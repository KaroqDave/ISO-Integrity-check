#include "core/verifier.h"

#include "core/checksum.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <memory>

#include <stdexcept>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef ISO_HAS_OPENSSL
#include <openssl/evp.h>
#endif

namespace iso {
namespace {

// A multiple of 4096 so the same buffers satisfy the alignment rules for
// unbuffered reads on 512-byte and 4K-sector volumes alike.
constexpr qint64 HashBufferSize = 8 * 1024 * 1024;

// Read buffers are allocated once and cycled between the reader and the hashing
// loop. Reading into a reused block, rather than letting QFile::read() hand back
// a fresh QByteArray, avoids an 8 MB allocation and free per chunk — on the
// order of a thousand of each for a typical ISO.
//
// Four slots let three reads stay outstanding while the fourth buffer is being
// hashed; on the unbuffered path that count *is* the queue depth handed to the
// drive. Measured against two slots on a local NVMe this changes nothing, since
// the reader is never the limit there — it is slack for media that answer
// unevenly, bought for 16 MB of resident memory.
constexpr size_t HashBufferSlots = 4;

struct FilledBuffer {
    size_t slot = 0;
    qint64 length = 0;
};

// Yields the file's bytes in order. nextChunk() returns false at end of file;
// the pointer it hands back stays valid until the following call, at which
// point the buffer goes back to the reader.
class ChunkSource {
  public:
    virtual ~ChunkSource() = default;
    virtual bool nextChunk(const char*& data, qint64& length) = 0;
    virtual qint64 size() const = 0;
};

struct HashCancelledException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void throwIfCancelled(const CancelToken& cancelToken)
{
    if (cancelToken && cancelToken->load()) {
        throw HashCancelledException("Verification was cancelled.");
    }
}

void throwOnReadError(const QFile& file)
{
    if (file.error() != QFileDevice::NoError) {
        throw std::runtime_error(
            QStringLiteral("The selected file could not be read: %1").arg(file.errorString()).toStdString());
    }
}

// Guards against a silently truncated read. QFile::read() returns an empty
// buffer both at EOF and when a driver hands back zero bytes without setting an
// error (removable media pulled mid-read, a dropped network share). Without this
// check the digest of a partial file would be reported as a legitimate result.
void throwOnIncompleteRead(qint64 bytesHashed, qint64 expectedSize)
{
    if (expectedSize <= 0 || bytesHashed == expectedSize) {
        return;
    }

    const QString detail = bytesHashed < expectedSize
                               ? QStringLiteral("only %1 of %2 bytes could be read").arg(bytesHashed).arg(expectedSize)
                               : QStringLiteral("%1 bytes were read but the file reported %2")
                                     .arg(bytesHashed)
                                     .arg(expectedSize);
    throw std::runtime_error(QStringLiteral("The file was not read completely (%1). It may have been modified, or the "
                                            "drive disconnected, during verification.")
                                 .arg(detail)
                                 .toStdString());
}

// Portable source: a reader thread fills buffers from a QFile while the caller
// hashes the ones already filled. Used everywhere the unbuffered source is
// unavailable, and whenever the caller asks for buffered I/O.
class ReadAheadSource final : public ChunkSource {
  public:
    ReadAheadSource(const QString& filePath, IoPolicy ioPolicy, const CancelToken& cancelToken)
        : file_(filePath), ioPolicy_(ioPolicy), cancelToken_(cancelToken),
          buffers_(HashBufferSlots, std::vector<char>(static_cast<size_t>(HashBufferSize)))
    {
        if (!file_.open(QIODevice::ReadOnly)) {
            throw std::runtime_error(
                QStringLiteral("The selected file could not be opened: %1").arg(file_.errorString()).toStdString());
        }
        size_ = file_.size();
        applySequentialHint();

        freeSlots_.reserve(HashBufferSlots);
        for (size_t slot = 0; slot < HashBufferSlots; ++slot) {
            freeSlots_.push_back(slot);
        }
        reader_ = std::thread([this]() { runReader(); });
    }

    ~ReadAheadSource() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopReader_ = true;
        }
        released_.notify_one();
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    ReadAheadSource(const ReadAheadSource&) = delete;
    ReadAheadSource& operator=(const ReadAheadSource&) = delete;

    bool nextChunk(const char*& data, qint64& length) override
    {
        releaseCurrentSlot();

        FilledBuffer chunk;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this]() { return !filled_.empty() || readFinished_ || readError_; });
            if (readError_) {
                std::rethrow_exception(readError_);
            }
            if (filled_.empty()) {
                return false;
            }
            chunk = filled_.front();
            filled_.pop_front();
        }

        currentSlot_ = chunk.slot;
        hasCurrentSlot_ = true;
        data = buffers_[chunk.slot].data();
        length = chunk.length;
        return true;
    }

    qint64 size() const override { return size_; }

  private:
    void releaseCurrentSlot()
    {
        if (!hasCurrentSlot_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            freeSlots_.push_back(currentSlot_);
        }
        hasCurrentSlot_ = false;
        released_.notify_one();
    }

    // Tells the kernel this is a front-to-back read so its own read-ahead lines
    // up with ours. Harmless where unsupported.
    void applySequentialHint()
    {
#ifdef Q_OS_UNIX
        const int descriptor = file_.handle();
        if (descriptor < 0) {
            return;
        }
#if defined(POSIX_FADV_SEQUENTIAL)
        posix_fadvise(descriptor, 0, 0, POSIX_FADV_SEQUENTIAL);
#elif defined(F_RDAHEAD)
        fcntl(descriptor, F_RDAHEAD, 1);
#endif
#endif
    }

    // Unbuffered I/O on Unix would mean O_DIRECT, which imposes alignment rules
    // that several filesystems (tmpfs, some network mounts, older overlayfs)
    // simply refuse. Dropping the pages right after copying them out reaches the
    // same goal — the cache does not fill up with ISO bytes — without the
    // portability cliff.
    void releasePageCache(qint64 offset, qint64 length)
    {
        if (ioPolicy_ != IoPolicy::Unbuffered) {
            return;
        }
#ifdef Q_OS_UNIX
        const int descriptor = file_.handle();
        if (descriptor < 0) {
            return;
        }
#if defined(POSIX_FADV_DONTNEED)
        posix_fadvise(descriptor, static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_DONTNEED);
#elif defined(F_NOCACHE)
        Q_UNUSED(offset);
        Q_UNUSED(length);
        fcntl(descriptor, F_NOCACHE, 1);
#else
        Q_UNUSED(offset);
        Q_UNUSED(length);
#endif
#else
        Q_UNUSED(offset);
        Q_UNUSED(length);
#endif
    }

    void runReader()
    {
        qint64 offset = 0;
        try {
            while (true) {
                size_t slot = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    released_.wait(lock, [this]() { return !freeSlots_.empty() || stopReader_; });
                    if (stopReader_) {
                        return;
                    }
                    slot = freeSlots_.back();
                    freeSlots_.pop_back();
                }

                const qint64 bytesRead = file_.read(buffers_[slot].data(), HashBufferSize);
                if (bytesRead <= 0) {
                    throwOnReadError(file_);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        readFinished_ = true;
                    }
                    ready_.notify_one();
                    return;
                }

                // Safe the instant read() returns: the bytes are in our buffer
                // now, so the cache's copy has no remaining reader.
                releasePageCache(offset, bytesRead);
                offset += bytesRead;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    filled_.push_back(FilledBuffer{slot, bytesRead});
                }
                ready_.notify_one();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                readError_ = std::current_exception();
                readFinished_ = true;
            }
            ready_.notify_one();
        }
    }

    QFile file_;
    IoPolicy ioPolicy_;
    CancelToken cancelToken_;
    std::vector<std::vector<char>> buffers_;
    qint64 size_ = 0;

    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable released_;
    std::deque<FilledBuffer> filled_;
    std::vector<size_t> freeSlots_;
    std::exception_ptr readError_;
    size_t currentSlot_ = 0;
    bool hasCurrentSlot_ = false;
    bool readFinished_ = false;
    bool stopReader_ = false;
};

#ifdef _WIN32

// Thrown when an unbuffered open is not possible for this file. The caller
// retries with buffered reads rather than failing the verification.
struct UnbufferedUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Reads with the page cache bypassed and several requests outstanding at once.
// The kernel transfers each block straight into our buffers, so nothing is
// copied by way of the cache, and verifying a multi-gigabyte ISO stops
// displacing whatever the user actually had cached.
//
// Unbuffered reads come with rules: buffer addresses, file offsets and request
// lengths must all be sector-aligned. VirtualAlloc returns page-aligned memory
// and HashBufferSize is a multiple of 4096, which covers 512-byte and 4K
// sectors alike; anything stranger makes the constructor bail out so the caller
// falls back. Requests are issued in slot order and waited on in the same
// order, which keeps delivery sequential while leaving the rest of the slots in
// flight.
class UnbufferedOverlappedSource final : public ChunkSource {
  public:
    UnbufferedOverlappedSource(const QString& filePath, const CancelToken& cancelToken) : cancelToken_(cancelToken)
    {
        try {
            open(filePath);
            for (size_t index = 0; index < HashBufferSlots; ++index) {
                issueRead(index);
            }
        } catch (...) {
            // The destructor does not run for a throwing constructor, and there
            // may already be reads in flight against buffers about to be freed.
            cleanup();
            throw;
        }
    }

    ~UnbufferedOverlappedSource() override { cleanup(); }

    UnbufferedOverlappedSource(const UnbufferedOverlappedSource&) = delete;
    UnbufferedOverlappedSource& operator=(const UnbufferedOverlappedSource&) = delete;

    bool nextChunk(const char*& data, qint64& length) override
    {
        // Re-arm the buffer the caller has finished with before waiting on the
        // next one, so the drive always has the other slots queued.
        if (hasCurrentSlot_) {
            hasCurrentSlot_ = false;
            issueRead(currentSlot_);
        }
        if (finished_) {
            return false;
        }

        SlotState& slot = slots_[nextSlot_];
        if (!slot.pending) {
            finished_ = true;
            return false;
        }

        const DWORD bytesRead = waitForSlot(slot);
        slot.pending = false;
        if (bytesRead == 0) {
            finished_ = true;
            return false;
        }

        data = bufferFor(nextSlot_);
        length = static_cast<qint64>(bytesRead);
        currentSlot_ = nextSlot_;
        hasCurrentSlot_ = true;
        nextSlot_ = (nextSlot_ + 1) % HashBufferSlots;
        return true;
    }

    qint64 size() const override { return size_; }

  private:
    struct SlotState {
        OVERLAPPED overlapped{};
        HANDLE event = nullptr;
        bool pending = false;
    };

    void open(const QString& filePath)
    {
        const QString nativePath = QDir::toNativeSeparators(filePath);
        handle_ = CreateFileW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw UnbufferedUnavailable("Unbuffered reads are not available for this file.");
        }

        // Compressed and encrypted NTFS files, and volumes with unusual
        // geometry, cannot satisfy our fixed buffer size.
        FILE_STORAGE_INFO storageInfo{};
        if (GetFileInformationByHandleEx(handle_, FileStorageInfo, &storageInfo, sizeof(storageInfo)) &&
            storageInfo.LogicalBytesPerSector != 0 && (HashBufferSize % storageInfo.LogicalBytesPerSector) != 0) {
            throw UnbufferedUnavailable("The volume sector size is incompatible with unbuffered reads.");
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(handle_, &fileSize)) {
            throw UnbufferedUnavailable("The file size could not be determined.");
        }
        size_ = static_cast<qint64>(fileSize.QuadPart);

        region_ = VirtualAlloc(
            nullptr, static_cast<SIZE_T>(HashBufferSize) * HashBufferSlots, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!region_) {
            throw UnbufferedUnavailable("The aligned read buffers could not be allocated.");
        }

        slots_.resize(HashBufferSlots);
        for (SlotState& slot : slots_) {
            slot.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!slot.event) {
                throw UnbufferedUnavailable("The read completion events could not be created.");
            }
        }
    }

    char* bufferFor(size_t index) const
    {
        return static_cast<char*>(region_) + (static_cast<size_t>(HashBufferSize) * index);
    }

    void issueRead(size_t index)
    {
        SlotState& slot = slots_[index];
        slot.pending = false;
        if (nextOffset_ >= size_) {
            return;
        }

        ZeroMemory(&slot.overlapped, sizeof(slot.overlapped));
        slot.overlapped.hEvent = slot.event;
        slot.overlapped.Offset = static_cast<DWORD>(static_cast<quint64>(nextOffset_) & 0xFFFFFFFFull);
        slot.overlapped.OffsetHigh = static_cast<DWORD>(static_cast<quint64>(nextOffset_) >> 32);
        ResetEvent(slot.event);

        if (!ReadFile(handle_, bufferFor(index), static_cast<DWORD>(HashBufferSize), nullptr, &slot.overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return;
            }
            if (error != ERROR_IO_PENDING) {
                throw std::runtime_error("The selected file could not be read.");
            }
        }

        slot.pending = true;
        // Overshooting the file size on the final short read is fine: it just
        // stops any further request from being issued.
        nextOffset_ += HashBufferSize;
    }

    DWORD waitForSlot(SlotState& slot)
    {
        // Polled rather than an infinite wait so cancelling stays responsive
        // even if the drive is slow to answer.
        while (true) {
            const DWORD waitResult = WaitForSingleObject(slot.event, 100);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            if (waitResult != WAIT_TIMEOUT) {
                throw std::runtime_error("Waiting for a file read to complete failed.");
            }
            throwIfCancelled(cancelToken_);
        }

        DWORD bytesRead = 0;
        if (!GetOverlappedResult(handle_, &slot.overlapped, &bytesRead, FALSE)) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                return 0;
            }
            throw std::runtime_error("The selected file could not be read.");
        }
        return bytesRead;
    }

    void cleanup()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            // Every outstanding read must be accounted for before the buffers go
            // away, or the kernel completes into freed memory.
            CancelIoEx(handle_, nullptr);
            for (SlotState& slot : slots_) {
                if (slot.pending) {
                    DWORD bytesRead = 0;
                    GetOverlappedResult(handle_, &slot.overlapped, &bytesRead, TRUE);
                    slot.pending = false;
                }
            }
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }

        for (SlotState& slot : slots_) {
            if (slot.event) {
                CloseHandle(slot.event);
                slot.event = nullptr;
            }
        }
        if (region_) {
            VirtualFree(region_, 0, MEM_RELEASE);
            region_ = nullptr;
        }
    }

    CancelToken cancelToken_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    void* region_ = nullptr;
    std::vector<SlotState> slots_;
    qint64 size_ = 0;
    qint64 nextOffset_ = 0;
    size_t nextSlot_ = 0;
    size_t currentSlot_ = 0;
    bool hasCurrentSlot_ = false;
    bool finished_ = false;
};

#endif // _WIN32

std::unique_ptr<ChunkSource> makeChunkSource(const QString& filePath, IoPolicy ioPolicy, const CancelToken& cancelToken)
{
#ifdef _WIN32
    if (ioPolicy == IoPolicy::Unbuffered) {
        try {
            return std::make_unique<UnbufferedOverlappedSource>(filePath, cancelToken);
        } catch (const UnbufferedUnavailable&) {
            // Compressed or encrypted files, some network redirectors and odd
            // sector geometries all reject unbuffered opens. Buffered reads work
            // everywhere, so drop back rather than fail the verification.
        }
    }
#endif
    return std::make_unique<ReadAheadSource>(filePath, ioPolicy, cancelToken);
}

// Feeds every chunk of the file to hashChunk in order. The chunk pointer is
// only valid for the duration of the call.
template <typename HashChunkFn>
void hashFileChunks(ChunkSource& source, HashChunkFn&& hashChunk, const CancelToken& cancelToken)
{
    qint64 totalBytesHashed = 0;
    const char* data = nullptr;
    qint64 length = 0;

    while (source.nextChunk(data, length)) {
        throwIfCancelled(cancelToken);
        totalBytesHashed += length;
        hashChunk(data, static_cast<qsizetype>(length));
    }

    throwIfCancelled(cancelToken);
    throwOnIncompleteRead(totalBytesHashed, source.size());
}

// One digest being fed the file's bytes. Wrapping each backend behind this lets
// a single read pass drive several algorithms at once, so computing SHA256 and
// SHA512 for an ISO costs one read instead of two.
class Hasher {
  public:
    virtual ~Hasher() = default;
    virtual void update(const char* data, qsizetype length) = 0;
    virtual QByteArray finish() = 0;
};

// Thrown when a native backend fails. The caller retries the whole pass with Qt
// hashing rather than leaving some digests native and others not.
struct NativeBackendError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#ifdef _WIN32

struct CngError : NativeBackendError {
    using NativeBackendError::NativeBackendError;
};

LPCWSTR cngAlgorithmId(const QString& algorithm)
{
    if (algorithm == QStringLiteral("SHA256")) {
        return BCRYPT_SHA256_ALGORITHM;
    }
    if (algorithm == QStringLiteral("SHA512")) {
        return BCRYPT_SHA512_ALGORITHM;
    }
    if (algorithm == QStringLiteral("SHA1")) {
        return BCRYPT_SHA1_ALGORITHM;
    }
    if (algorithm == QStringLiteral("MD5")) {
        return BCRYPT_MD5_ALGORITHM;
    }
    return nullptr;
}

class CngHasher final : public Hasher {
  public:
    explicit CngHasher(LPCWSTR algorithmId)
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, algorithmId, nullptr, 0) < 0) {
            throw CngError("Failed to open the system hash provider.");
        }
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw CngError("Failed to create the hash object.");
        }
    }

    ~CngHasher() override
    {
        if (hash_) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    CngHasher(const CngHasher&) = delete;
    CngHasher& operator=(const CngHasher&) = delete;

    void update(const char* data, qsizetype length) override
    {
        if (length <= 0) {
            return;
        }
        if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(length), 0) <
            0) {
            throw CngError("Failed while hashing file data.");
        }
    }

    QByteArray finish() override
    {
        DWORD hashLength = 0;
        DWORD written = 0;
        if (BCryptGetProperty(
                hash_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &written, 0) <
            0) {
            throw CngError("Failed to query the hash length.");
        }

        QByteArray result(static_cast<qsizetype>(hashLength), '\0');
        if (BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(result.data()), hashLength, 0) < 0) {
            throw CngError("Failed to finalize the hash.");
        }
        return result;
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

#endif // _WIN32

#ifdef ISO_HAS_OPENSSL

struct EvpError : NativeBackendError {
    using NativeBackendError::NativeBackendError;
};

const EVP_MD* evpAlgorithm(const QString& algorithm)
{
    if (algorithm == QStringLiteral("SHA256")) {
        return EVP_sha256();
    }
    if (algorithm == QStringLiteral("SHA512")) {
        return EVP_sha512();
    }
    if (algorithm == QStringLiteral("SHA1")) {
        return EVP_sha1();
    }
    if (algorithm == QStringLiteral("MD5")) {
        return EVP_md5();
    }
    return nullptr;
}

class EvpHasher final : public Hasher {
  public:
    explicit EvpHasher(const EVP_MD* algorithm) : context_(EVP_MD_CTX_new())
    {
        if (!context_) {
            throw EvpError("Failed to create the OpenSSL hash context.");
        }
        if (EVP_DigestInit_ex(context_, algorithm, nullptr) != 1) {
            throw EvpError("Failed to initialize the OpenSSL hash.");
        }
    }

    ~EvpHasher() override { EVP_MD_CTX_free(context_); }

    EvpHasher(const EvpHasher&) = delete;
    EvpHasher& operator=(const EvpHasher&) = delete;

    void update(const char* data, qsizetype length) override
    {
        if (length <= 0) {
            return;
        }
        if (EVP_DigestUpdate(context_, data, static_cast<size_t>(length)) != 1) {
            throw EvpError("Failed while hashing file data with OpenSSL.");
        }
    }

    QByteArray finish() override
    {
        QByteArray result(EVP_MAX_MD_SIZE, '\0');
        unsigned int hashLength = 0;
        if (EVP_DigestFinal_ex(context_, reinterpret_cast<unsigned char*>(result.data()), &hashLength) != 1) {
            throw EvpError("Failed to finalize the OpenSSL hash.");
        }
        result.resize(static_cast<qsizetype>(hashLength));
        return result;
    }

  private:
    EVP_MD_CTX* context_ = nullptr;
};

#endif // ISO_HAS_OPENSSL

class QtHasher final : public Hasher {
  public:
    explicit QtHasher(QCryptographicHash::Algorithm algorithm) : digest_(algorithm) {}

    void update(const char* data, qsizetype length) override
    {
        if (length <= 0) {
            return;
        }
        digest_.addData(QByteArrayView(data, length));
    }

    QByteArray finish() override { return digest_.result(); }

  private:
    QCryptographicHash digest_;
};

// Returns nullptr when no native backend covers this algorithm, in which case
// the caller falls back to Qt. Construction failures surface as
// NativeBackendError so the whole pass restarts on Qt hashing.
std::unique_ptr<Hasher> makeNativeHasher(const QString& algorithm)
{
#ifdef _WIN32
    if (LPCWSTR algorithmId = cngAlgorithmId(algorithm)) {
        return std::make_unique<CngHasher>(algorithmId);
    }
#endif
#ifdef ISO_HAS_OPENSSL
    if (const EVP_MD* algorithmId = evpAlgorithm(algorithm)) {
        return std::make_unique<EvpHasher>(algorithmId);
    }
#endif
    Q_UNUSED(algorithm);
    return nullptr;
}

struct NamedHasher {
    QString algorithm;
    std::unique_ptr<Hasher> hasher;
};

// Feeds one chunk to every digest at once.
//
// A single digest cannot be split across cores — Merkle-Damgard chaining makes
// block N+1 depend on block N — but separate algorithms are independent of each
// other. Running them concurrently makes SHA256 + SHA512 cost the slower of the
// two rather than their sum. The first digest runs on the calling thread and
// the rest get a worker each, so the common single-algorithm case spawns no
// threads and pays no synchronisation.
class ParallelDigest {
  public:
    explicit ParallelDigest(std::vector<NamedHasher>& hashers) : hashers_(hashers), errors_(hashers.size())
    {
        if (hashers_.size() <= 1) {
            return;
        }

        workers_.reserve(hashers_.size() - 1);
        for (size_t index = 1; index < hashers_.size(); ++index) {
            workers_.emplace_back([this, index]() { runWorker(index); });
        }
    }

    ~ParallelDigest()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ParallelDigest(const ParallelDigest&) = delete;
    ParallelDigest& operator=(const ParallelDigest&) = delete;

    void update(const char* data, qsizetype length)
    {
        if (length <= 0) {
            return;
        }
        if (workers_.empty()) {
            hashers_.front().hasher->update(data, length);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            data_ = data;
            length_ = length;
            pending_ = workers_.size();
            ++generation_;
        }
        work_.notify_all();

        try {
            hashers_.front().hasher->update(data, length);
        } catch (...) {
            errors_.front() = std::current_exception();
        }

        // Waiting is not optional even when the inline digest just failed: the
        // workers are still reading the caller's buffer, and it is handed back
        // to the reader thread the moment update() returns.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            done_.wait(lock, [this]() { return pending_ == 0; });
        }

        rethrowFirstError();
    }

  private:
    void runWorker(size_t index)
    {
        quint64 seenGeneration = 0;
        while (true) {
            const char* data = nullptr;
            qsizetype length = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_.wait(lock, [this, seenGeneration]() { return stop_ || generation_ != seenGeneration; });
                if (stop_) {
                    return;
                }
                seenGeneration = generation_;
                data = data_;
                length = length_;
            }

            if (!errors_[index]) {
                try {
                    hashers_[index].hasher->update(data, length);
                } catch (...) {
                    errors_[index] = std::current_exception();
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --pending_;
            }
            done_.notify_one();
        }
    }

    // Safe to read unlocked once the barrier has cleared: every worker writes
    // its slot before taking the mutex to decrement pending_.
    void rethrowFirstError()
    {
        for (std::exception_ptr& error : errors_) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
    }

    std::vector<NamedHasher>& hashers_;
    std::vector<std::exception_ptr> errors_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable done_;
    const char* data_ = nullptr;
    qsizetype length_ = 0;
    quint64 generation_ = 0;
    size_t pending_ = 0;
    bool stop_ = false;
};

// Reads the file once and feeds every chunk to all requested digests.
QHash<QString, QString> hashInOnePass(
    ChunkSource& source,
    const QStringList& algorithms,
    bool useNativeBackends,
    const ProgressCallback& progressCallback,
    const CancelToken& cancelToken)
{
    const auto& hashes = supportedHashes();
    std::vector<NamedHasher> hashers;
    hashers.reserve(static_cast<size_t>(algorithms.size()));

    for (const QString& algorithm : algorithms) {
        std::unique_ptr<Hasher> hasher;
        if (useNativeBackends) {
            hasher = makeNativeHasher(algorithm);
        }
        if (!hasher) {
            hasher = std::make_unique<QtHasher>(hashes.value(algorithm).qtAlgorithm);
        }
        hashers.push_back(NamedHasher{algorithm, std::move(hasher)});
    }

    qint64 bytesRead = 0;
    {
        // Scoped so the worker threads are joined before finish() touches the
        // hashers below.
        ParallelDigest digests(hashers);
        hashFileChunks(
            source,
            [&](const char* data, qsizetype length) {
                digests.update(data, length);
                bytesRead += length;
                if (progressCallback) {
                    progressCallback(bytesRead);
                }
            },
            cancelToken);
    }

    QHash<QString, QString> results;
    results.reserve(static_cast<qsizetype>(hashers.size()));
    for (NamedHasher& entry : hashers) {
        results.insert(entry.algorithm, QString::fromLatin1(entry.hasher->finish().toHex()));
    }
    return results;
}

// Drops duplicates while preserving the caller's order, and rejects unknown names.
QStringList normalizeAlgorithmList(const QStringList& algorithms)
{
    QStringList unique;
    for (const QString& algorithm : algorithms) {
        if (!supportedHashes().contains(algorithm)) {
            throw std::runtime_error(QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm).toStdString());
        }
        if (!unique.contains(algorithm)) {
            unique.append(algorithm);
        }
    }
    if (unique.isEmpty()) {
        throw std::runtime_error("No hash algorithm was requested.");
    }
    return unique;
}

} // namespace

QHash<QString, QString> calculateFileHashes(
    const QString& filePath,
    const QStringList& algorithms,
    ProgressCallback progressCallback,
    CancelToken cancelToken,
    IoPolicy ioPolicy)
{
    const QStringList requested = normalizeAlgorithmList(algorithms);

    try {
        auto source = makeChunkSource(filePath, ioPolicy, cancelToken);
        return hashInOnePass(*source, requested, true, progressCallback, cancelToken);
    } catch (const NativeBackendError&) {
        // Restart the whole pass on Qt hashing, from a fresh source. The
        // listener is told progress rewound, otherwise it keeps comparing
        // against the byte count from the abandoned attempt and its throughput
        // estimate stalls.
        auto source = makeChunkSource(filePath, ioPolicy, cancelToken);
        if (progressCallback) {
            progressCallback(0);
        }
        return hashInOnePass(*source, requested, false, progressCallback, cancelToken);
    }
}

QString calculateFileHash(
    const QString& filePath,
    const QString& algorithm,
    ProgressCallback progressCallback,
    CancelToken cancelToken,
    IoPolicy ioPolicy)
{
    return calculateFileHashes(
               filePath, {algorithm}, std::move(progressCallback), std::move(cancelToken), ioPolicy)
        .value(algorithm);
}

VerificationResult verifyChecksum(
    const QString& filePath,
    const QString& expectedChecksum,
    const QString& algorithm,
    ProgressCallback progressCallback,
    CancelToken cancelToken,
    const QStringList& alsoCompute,
    IoPolicy ioPolicy)
{
    const QFileInfo info(filePath);
    if (filePath.isEmpty()) {
        return {VerificationStatus::Error, QStringLiteral("Choose an ISO file first."), {}, std::nullopt};
    }

    if (!supportedHashes().contains(algorithm)) {
        return {
            VerificationStatus::Error,
            QStringLiteral("Unsupported hash algorithm: %1").arg(algorithm),
            {},
            std::nullopt};
    }

    if (!info.exists()) {
        return {VerificationStatus::Error, QStringLiteral("The selected file does not exist."), {}, std::nullopt};
    }

    if (!info.isFile()) {
        return {VerificationStatus::Error, QStringLiteral("The selected path is not a file."), {}, std::nullopt};
    }

    const auto validationError = validateExpectedChecksum(expectedChecksum, algorithm);
    if (validationError.has_value()) {
        return {VerificationStatus::Error, *validationError, {}, std::nullopt};
    }

    const QString normalizedExpected = normalizeChecksum(expectedChecksum);

    QString computedHash;
    QHash<QString, QString> computedHashes;
    try {
        QStringList requested{algorithm};
        for (const QString& extra : alsoCompute) {
            if (supportedHashes().contains(extra) && !requested.contains(extra)) {
                requested.append(extra);
            }
        }
        computedHashes =
            calculateFileHashes(filePath, requested, std::move(progressCallback), cancelToken, ioPolicy);
        computedHash = computedHashes.value(algorithm);
    } catch (const HashCancelledException&) {
        return {
            VerificationStatus::Cancelled,
            QStringLiteral("Verification cancelled."),
            {},
            std::nullopt,
        };
    } catch (const std::exception& error) {
        return {
            VerificationStatus::Error,
            QString::fromUtf8(error.what()),
            {},
            std::nullopt,
        };
    }

    if (normalizedExpected.isEmpty()) {
        return {
            VerificationStatus::Generated,
            QStringLiteral("Checksum calculated. Paste or import an official checksum to verify integrity."),
            computedHash,
            std::nullopt,
            computedHashes,
        };
    }

    if (computedHash == normalizedExpected) {
        return {
            VerificationStatus::Match,
            QStringLiteral("The ISO checksum matches the expected value."),
            computedHash,
            true,
            computedHashes,
        };
    }

    return {
        VerificationStatus::Mismatch,
        QStringLiteral("The ISO checksum does not match the expected value."),
        computedHash,
        false,
        computedHashes,
    };
}

namespace bench {

qint64 readOnlyPass(const QString& filePath, bool useUnbufferedIo, CancelToken cancelToken)
{
    auto source = makeChunkSource(filePath, useUnbufferedIo ? IoPolicy::Unbuffered : IoPolicy::Buffered, cancelToken);

    qint64 bytesRead = 0;
    hashFileChunks(
        *source, [&](const char*, qsizetype length) { bytesRead += length; }, cancelToken);
    return bytesRead;
}

QHash<QString, QString> hashWithBackend(
    const QString& filePath,
    const QStringList& algorithms,
    bool useNativeBackends,
    bool useUnbufferedIo,
    CancelToken cancelToken)
{
    const QStringList requested = normalizeAlgorithmList(algorithms);
    auto source = makeChunkSource(filePath, useUnbufferedIo ? IoPolicy::Unbuffered : IoPolicy::Buffered, cancelToken);
    return hashInOnePass(*source, requested, useNativeBackends, {}, cancelToken);
}

bool unbufferedIoAvailable(const QString& filePath)
{
#ifdef _WIN32
    try {
        UnbufferedOverlappedSource probe(filePath, {});
        return true;
    } catch (const UnbufferedUnavailable&) {
        return false;
    }
#else
    Q_UNUSED(filePath);
    return false;
#endif
}

} // namespace bench

QString formatStatusMessage(VerificationStatus status, const QString& message)
{
    switch (status) {
    case VerificationStatus::Match:
        return QStringLiteral("Match: %1").arg(message);
    case VerificationStatus::Mismatch:
        return QStringLiteral("Mismatch: %1").arg(message);
    case VerificationStatus::Error:
        return QStringLiteral("Error: %1").arg(message);
    case VerificationStatus::Generated:
        return message;
    case VerificationStatus::Cancelled:
        return QStringLiteral("Cancelled: %1").arg(message);
    }
    return message;
}

} // namespace iso
