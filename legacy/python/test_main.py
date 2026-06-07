import hashlib
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from main import (
    calculate_file_hash,
    load_checksum_file,
    parse_checksum_text,
    validate_expected_checksum,
    verify_checksum,
)

LEGACY_ROOT = Path(__file__).resolve().parents[1]
CLI_SCRIPT = LEGACY_ROOT / "cli" / "iso-integrity-check.ps1"
POWERSHELL = shutil.which("powershell.exe") or shutil.which("powershell")


class HashVerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.file_path = Path(self.temp_dir.name) / "sample.iso"
        self.file_path.write_bytes(b"iso integrity test data")

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_calculates_sha256_sha512_sha1_and_md5(self):
        data = self.file_path.read_bytes()

        self.assertEqual(calculate_file_hash(self.file_path, "SHA256"), hashlib.sha256(data).hexdigest())
        self.assertEqual(calculate_file_hash(self.file_path, "SHA512"), hashlib.sha512(data).hexdigest())
        self.assertEqual(calculate_file_hash(self.file_path, "SHA1"), hashlib.sha1(data).hexdigest())
        self.assertEqual(calculate_file_hash(self.file_path, "MD5"), hashlib.md5(data).hexdigest())

    def test_expected_hash_is_case_insensitive_and_trimmed(self):
        expected = hashlib.sha256(self.file_path.read_bytes()).hexdigest().upper()

        result = verify_checksum(self.file_path, f"  {expected}\n", "SHA256")

        self.assertEqual(result.status, "match")
        self.assertTrue(result.matches)

    def test_mismatch_is_reported(self):
        wrong_hash = "0" * 64

        result = verify_checksum(self.file_path, wrong_hash, "SHA256")

        self.assertEqual(result.status, "mismatch")
        self.assertFalse(result.matches)

    def test_missing_checksum_generates_hash(self):
        result = verify_checksum(self.file_path, "", "SHA256")

        self.assertEqual(result.status, "generated")
        self.assertIsNone(result.matches)
        self.assertEqual(len(result.computed_hash), 64)

    def test_invalid_checksum_length_is_rejected_before_hashing(self):
        error = validate_expected_checksum("abc", "SHA256")

        self.assertIn("64 hexadecimal characters", error)

    def test_invalid_checksum_characters_are_rejected(self):
        error = validate_expected_checksum("g" * 64, "SHA256")

        self.assertIn("hexadecimal", error)

    def test_sha1_validation_uses_40_hex_characters(self):
        error = validate_expected_checksum("a" * 39, "SHA1")

        self.assertIn("40 hexadecimal characters", error)

    def test_missing_file_is_reported(self):
        missing_path = Path(self.temp_dir.name) / "missing.iso"

        result = verify_checksum(missing_path, "0" * 64, "SHA256")

        self.assertEqual(result.status, "error")
        self.assertIn("does not exist", result.message)


class ChecksumFileParsingTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.iso_path = Path(self.temp_dir.name) / "sample.iso"
        self.iso_path.write_bytes(b"iso integrity test data")

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_plain_sha256_file_is_parsed(self):
        checksum = hashlib.sha256(self.iso_path.read_bytes()).hexdigest()

        parsed = parse_checksum_text(checksum, iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "SHA256")
        self.assertEqual(parsed.checksum, checksum)
        self.assertEqual(parsed.line_number, 1)

    def test_gnu_style_line_with_filename_is_parsed(self):
        checksum = hashlib.sha512(self.iso_path.read_bytes()).hexdigest()

        parsed = parse_checksum_text(f"{checksum}  sample.iso", iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "SHA512")
        self.assertEqual(parsed.checksum, checksum)
        self.assertEqual(parsed.filename, "sample.iso")

    def test_gnu_binary_marker_filename_is_parsed(self):
        checksum = hashlib.md5(self.iso_path.read_bytes()).hexdigest()

        parsed = parse_checksum_text(f"{checksum} *sample.iso", iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "MD5")
        self.assertEqual(parsed.filename, "sample.iso")

    def test_matching_iso_filename_wins_over_first_supported_hash(self):
        wrong_file_hash = "0" * 64
        matching_hash = hashlib.sha256(self.iso_path.read_bytes()).hexdigest()
        text = f"{wrong_file_hash}  other.iso\n{matching_hash}  sample.iso\n"

        parsed = parse_checksum_text(text, iso_path=self.iso_path)

        self.assertEqual(parsed.checksum, matching_hash)
        self.assertEqual(parsed.filename, "sample.iso")

    def test_exact_iso_filename_wins_over_partial_filename_match(self):
        wrong_file_hash = "0" * 64
        matching_hash = hashlib.sha256(self.iso_path.read_bytes()).hexdigest()
        text = f"{wrong_file_hash}  sample.iso.zsync\n{matching_hash}  sample.iso\n"

        parsed = parse_checksum_text(text, iso_path=self.iso_path)

        self.assertEqual(parsed.checksum, matching_hash)
        self.assertEqual(parsed.filename, "sample.iso")

    def test_raw_line_mentions_do_not_count_as_filename_matches(self):
        wrong_file_hash = "0" * 64
        matching_hash = hashlib.sha256(self.iso_path.read_bytes()).hexdigest()
        text = f"{wrong_file_hash}  other.iso # sample.iso\n{matching_hash}  sample.iso\n"

        parsed = parse_checksum_text(text, iso_path=self.iso_path)

        self.assertEqual(parsed.checksum, matching_hash)
        self.assertEqual(parsed.filename, "sample.iso")

    def test_first_supported_hash_is_used_without_filename_match(self):
        first_hash = "1" * 40
        second_hash = "2" * 64
        text = f"{first_hash}  other.iso\n{second_hash}  another.iso\n"

        parsed = parse_checksum_text(text, iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "SHA1")
        self.assertEqual(parsed.checksum, first_hash)

    def test_unsupported_or_malformed_file_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "No supported checksum"):
            parse_checksum_text("not a checksum\nalso not a checksum", iso_path=self.iso_path)

    def test_load_checksum_file_reads_and_parses_file(self):
        checksum = hashlib.sha1(self.iso_path.read_bytes()).hexdigest()
        checksum_file = Path(self.temp_dir.name) / "sample.sha1"
        checksum_file.write_text(f"{checksum}  sample.iso", encoding="utf-8")

        parsed = load_checksum_file(checksum_file, iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "SHA1")
        self.assertEqual(parsed.checksum, checksum)

    def test_load_checksum_file_rejects_oversized_files(self):
        checksum_file = Path(self.temp_dir.name) / "large.sha256"
        checksum_file.write_text("0" * 64, encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "too large"):
            load_checksum_file(checksum_file, iso_path=self.iso_path, max_size=32)

    def test_bsd_style_line_with_filename_is_parsed(self):
        checksum = hashlib.sha256(self.iso_path.read_bytes()).hexdigest()

        parsed = parse_checksum_text(f"SHA256 (sample.iso) = {checksum}", iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "SHA256")
        self.assertEqual(parsed.checksum, checksum)
        self.assertEqual(parsed.filename, "sample.iso")

    def test_matching_bsd_style_filename_wins_over_first_supported_hash(self):
        wrong_file_hash = "0" * 64
        matching_hash = hashlib.sha512(self.iso_path.read_bytes()).hexdigest()
        text = f"SHA256 (other.iso) = {wrong_file_hash}\nSHA512 (sample.iso) = {matching_hash}\n"

        parsed = parse_checksum_text(text, iso_path=self.iso_path)

        self.assertEqual(parsed.algorithm, "SHA512")
        self.assertEqual(parsed.checksum, matching_hash)
        self.assertEqual(parsed.filename, "sample.iso")

    def test_parsed_checksum_can_verify_a_match(self):
        checksum = hashlib.sha256(self.iso_path.read_bytes()).hexdigest()
        parsed = parse_checksum_text(f"{checksum}  sample.iso", iso_path=self.iso_path)

        result = verify_checksum(self.iso_path, parsed.checksum, parsed.algorithm)

        self.assertEqual(result.status, "match")
        self.assertTrue(result.matches)

    def test_parsed_checksum_can_verify_a_mismatch(self):
        parsed = parse_checksum_text(f"{'0' * 64}  sample.iso", iso_path=self.iso_path)

        result = verify_checksum(self.iso_path, parsed.checksum, parsed.algorithm)

        self.assertEqual(result.status, "mismatch")
        self.assertFalse(result.matches)


@unittest.skipIf(POWERSHELL is None, "Windows PowerShell is not available")
class PowerShellCliTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.file_path = Path(self.temp_dir.name) / "sample.iso"
        self.file_path.write_bytes(b"iso integrity test data")

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_cli(self, *args):
        command = [
            POWERSHELL,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(CLI_SCRIPT),
            *[str(arg) for arg in args],
        ]
        return subprocess.run(command, capture_output=True, text=True)

    def test_cli_direct_checksum_match_with_inferred_algorithm(self):
        checksum = hashlib.sha256(self.file_path.read_bytes()).hexdigest()

        completed = self.run_cli("-File", self.file_path, "-Expected", checksum)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("MATCH", completed.stdout)
        self.assertIn(checksum, completed.stdout)

    def test_cli_direct_checksum_mismatch_exits_with_one(self):
        completed = self.run_cli("-File", self.file_path, "-Algorithm", "SHA256", "-Expected", "0" * 64)

        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        self.assertIn("MISMATCH", completed.stdout)

    def test_cli_hash_only_outputs_selected_algorithm_hash(self):
        checksum = hashlib.md5(self.file_path.read_bytes()).hexdigest()

        completed = self.run_cli("-File", self.file_path, "-Algorithm", "MD5")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Algorithm: MD5", completed.stdout)
        self.assertIn(checksum, completed.stdout)
        self.assertIn("Checksum calculated", completed.stdout)

    def test_cli_checksum_file_prefers_matching_filename(self):
        matching_checksum = hashlib.sha256(self.file_path.read_bytes()).hexdigest()
        checksum_file = Path(self.temp_dir.name) / "SHA256SUMS"
        checksum_file.write_text(f"{'0' * 64}  other.iso\n{matching_checksum}  sample.iso\n", encoding="utf-8")

        completed = self.run_cli("-File", self.file_path, "-ChecksumFile", checksum_file)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("MATCH", completed.stdout)
        self.assertIn("sample.iso", completed.stdout)

    def test_cli_parses_bsd_style_checksum_file(self):
        checksum = hashlib.sha512(self.file_path.read_bytes()).hexdigest()
        checksum_file = Path(self.temp_dir.name) / "sample.sha512"
        checksum_file.write_text(f"SHA512 (sample.iso) = {checksum}\n", encoding="utf-8")

        completed = self.run_cli("-File", self.file_path, "-ChecksumFile", checksum_file)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Algorithm: SHA512", completed.stdout)
        self.assertIn("MATCH", completed.stdout)

    def test_cli_rejects_invalid_checksum_length_and_characters(self):
        cases = [
            (["-File", self.file_path, "-Algorithm", "SHA256", "-Expected", "abc"], "64 hexadecimal characters"),
            (["-File", self.file_path, "-Algorithm", "SHA256", "-Expected", "g" * 64], "hexadecimal"),
        ]

        for args, expected_message in cases:
            with self.subTest(expected_message=expected_message):
                completed = self.run_cli(*args)

                self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
                self.assertIn(expected_message, completed.stderr)

    def test_cli_reports_missing_iso_and_checksum_files(self):
        missing_iso = Path(self.temp_dir.name) / "missing.iso"
        missing_checksum = Path(self.temp_dir.name) / "missing.sha256"
        cases = [
            (["-File", missing_iso, "-Algorithm", "SHA256", "-Expected", "0" * 64], "does not exist"),
            (["-File", self.file_path, "-ChecksumFile", missing_checksum], "checksum file"),
        ]

        for args, expected_message in cases:
            with self.subTest(expected_message=expected_message):
                completed = self.run_cli(*args)

                self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
                self.assertIn(expected_message, completed.stderr)


if __name__ == "__main__":
    unittest.main()
