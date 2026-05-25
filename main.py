from __future__ import annotations

import hashlib
import queue
import re
import threading
from dataclasses import dataclass
from pathlib import Path
from tkinter import (
    DISABLED,
    END,
    INSERT,
    Menu,
    NORMAL,
    StringVar,
    Tk,
    filedialog,
    messagebox,
    ttk,
)


SUPPORTED_HASHES = {
    "SHA256": {"hashlib_name": "sha256", "hex_length": 64, "legacy": False},
    "SHA512": {"hashlib_name": "sha512", "hex_length": 128, "legacy": False},
    "SHA1": {"hashlib_name": "sha1", "hex_length": 40, "legacy": True},
    "MD5": {"hashlib_name": "md5", "hex_length": 32, "legacy": True},
}

HASHES_BY_LENGTH = {
    details["hex_length"]: algorithm for algorithm, details in SUPPORTED_HASHES.items()
}

HEX_PATTERN = re.compile(r"^[0-9a-f]+$")
CHECKSUM_TOKEN_PATTERN = re.compile(r"(?<![0-9a-fA-F])([0-9a-fA-F]{128}|[0-9a-fA-F]{64}|[0-9a-fA-F]{40}|[0-9a-fA-F]{32})(?![0-9a-fA-F])")
CHUNK_SIZE = 4 * 1024 * 1024
APP_AUTHOR = "KaroqDave"
APP_PROFILE_URL = "https://github.com/KaroqDave"


@dataclass(frozen=True)
class VerificationResult:
    status: str
    message: str
    computed_hash: str = ""
    matches: bool | None = None


@dataclass(frozen=True)
class ParsedChecksum:
    algorithm: str
    checksum: str
    line_number: int
    filename: str = ""
    raw_line: str = ""


def normalize_checksum(value: str) -> str:
    return value.strip().lower()


def validate_expected_checksum(expected_checksum: str, algorithm: str) -> str | None:
    normalized = normalize_checksum(expected_checksum)
    if not normalized:
        return None

    if algorithm not in SUPPORTED_HASHES:
        return f"Unsupported hash algorithm: {algorithm}"

    expected_length = SUPPORTED_HASHES[algorithm]["hex_length"]
    if len(normalized) != expected_length:
        return (
            f"{algorithm} checksums must be {expected_length} hexadecimal characters. "
            f"The pasted value has {len(normalized)}."
        )

    if not HEX_PATTERN.fullmatch(normalized):
        return f"{algorithm} checksums can only contain hexadecimal characters."

    return None


def calculate_file_hash(
    file_path: str | Path,
    algorithm: str,
    chunk_size: int = CHUNK_SIZE,
    progress_callback: callable | None = None,
) -> str:
    if algorithm not in SUPPORTED_HASHES:
        raise ValueError(f"Unsupported hash algorithm: {algorithm}")

    path = Path(file_path)
    digest = hashlib.new(SUPPORTED_HASHES[algorithm]["hashlib_name"])
    bytes_read = 0

    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(chunk_size), b""):
            digest.update(chunk)
            bytes_read += len(chunk)
            if progress_callback is not None:
                progress_callback(bytes_read)

    return digest.hexdigest()


def parse_checksum_line(line: str, line_number: int) -> ParsedChecksum | None:
    stripped = line.strip()
    if not stripped or stripped.startswith(("#", ";")):
        return None

    match = CHECKSUM_TOKEN_PATTERN.search(stripped)
    if not match:
        return None

    checksum = match.group(1).lower()
    algorithm = HASHES_BY_LENGTH[len(checksum)]
    filename = _extract_checksum_filename(stripped, match.start(), match.end())
    return ParsedChecksum(
        algorithm=algorithm,
        checksum=checksum,
        line_number=line_number,
        filename=filename,
        raw_line=stripped,
    )


def parse_checksum_text(text: str, iso_path: str | Path | None = None) -> ParsedChecksum:
    candidates = [
        parsed
        for line_number, line in enumerate(text.splitlines(), start=1)
        if (parsed := parse_checksum_line(line, line_number)) is not None
    ]

    if not candidates:
        raise ValueError("No supported checksum was found in the selected file.")

    iso_name = Path(iso_path).name.lower() if iso_path else ""
    if iso_name:
        for candidate in candidates:
            if _checksum_candidate_matches_iso(candidate, iso_name):
                return candidate

    return candidates[0]


def load_checksum_file(checksum_file_path: str | Path, iso_path: str | Path | None = None) -> ParsedChecksum:
    path = Path(checksum_file_path)
    if not path.exists():
        raise ValueError("The selected checksum file does not exist.")
    if not path.is_file():
        raise ValueError("The selected checksum path is not a file.")

    text = path.read_text(encoding="utf-8-sig", errors="replace")
    return parse_checksum_text(text, iso_path=iso_path)


def verify_checksum(file_path: str | Path, expected_checksum: str, algorithm: str) -> VerificationResult:
    path = Path(file_path)
    if not file_path:
        return VerificationResult("error", "Choose an ISO file first.")

    if algorithm not in SUPPORTED_HASHES:
        return VerificationResult("error", f"Unsupported hash algorithm: {algorithm}")

    if not path.exists():
        return VerificationResult("error", "The selected file does not exist.")

    if not path.is_file():
        return VerificationResult("error", "The selected path is not a file.")

    validation_error = validate_expected_checksum(expected_checksum, algorithm)
    if validation_error:
        return VerificationResult("error", validation_error)

    normalized_expected = normalize_checksum(expected_checksum)
    computed_hash = calculate_file_hash(path, algorithm)

    if not normalized_expected:
        return VerificationResult(
            "generated",
            "Checksum calculated. Paste or import an official checksum to verify integrity.",
            computed_hash=computed_hash,
            matches=None,
        )

    if computed_hash == normalized_expected:
        return VerificationResult(
            "match",
            "Match. The ISO checksum matches the expected value.",
            computed_hash=computed_hash,
            matches=True,
        )

    return VerificationResult(
        "mismatch",
        "Mismatch. The ISO checksum does not match the expected value.",
        computed_hash=computed_hash,
        matches=False,
    )


def _extract_checksum_filename(line: str, token_start: int, token_end: int) -> str:
    before = line[:token_start].strip()
    after = line[token_end:].strip()

    filename = after or before
    if not filename:
        return ""

    filename = filename.lstrip("*").strip()
    if filename.startswith("(") and ")" in filename:
        filename = filename[1:filename.index(")")]
    if filename.startswith("="):
        filename = filename[1:].strip()

    return filename.strip().strip('"')


def _checksum_candidate_matches_iso(candidate: ParsedChecksum, iso_name: str) -> bool:
    filename = candidate.filename.replace("\\", "/").split("/")[-1].lower()
    return filename == iso_name or iso_name in candidate.raw_line.lower()


class ISOIntegrityApp:
    def __init__(self, root: Tk) -> None:
        self.root = root
        self.root.title("ISO Integrity Check")
        self.root.minsize(820, 560)

        self.file_path = StringVar()
        self.algorithm = StringVar(value="SHA256")
        self.status_text = StringVar(value="Select an ISO file, then paste or import a checksum.")
        self.detail_text = StringVar(value="Ready")
        self.worker_messages: queue.Queue[VerificationResult | Exception] = queue.Queue()

        self.styles = self._configure_styles()
        self._build_ui()

    def _configure_styles(self) -> ttk.Style:
        style = ttk.Style(self.root)
        if "vista" in style.theme_names():
            style.theme_use("vista")

        default_font = ("Segoe UI", 10)
        style.configure(".", font=default_font)
        style.configure("App.TFrame", background="#f5f7fb")
        style.configure("Header.TFrame", background="#f5f7fb")
        style.configure("Card.TLabelframe", background="#ffffff", borderwidth=1, relief="solid")
        style.configure("Card.TLabelframe.Label", font=("Segoe UI", 10, "bold"), foreground="#243044", background="#f5f7fb")
        style.configure("Title.TLabel", font=("Segoe UI", 22, "bold"), foreground="#172033", background="#f5f7fb")
        style.configure("Subtitle.TLabel", font=("Segoe UI", 10), foreground="#5f6b7a", background="#f5f7fb")
        style.configure("Muted.TLabel", foreground="#637083", background="#f5f7fb")
        style.configure("CardText.TLabel", foreground="#243044", background="#ffffff")
        style.configure("Status.TLabel", font=("Segoe UI", 11, "bold"), foreground="#243044", background="#ffffff")
        style.configure("Detail.TLabel", foreground="#637083", background="#ffffff")
        style.configure("Primary.TButton", padding=(18, 8))
        style.configure("Tool.TButton", padding=(12, 6))
        return style

    def _build_ui(self) -> None:
        self.root.configure(background="#f5f7fb")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        main = ttk.Frame(self.root, padding=24, style="App.TFrame")
        main.grid(row=0, column=0, sticky="nsew")
        main.columnconfigure(0, weight=1)
        main.rowconfigure(5, weight=1)

        header = ttk.Frame(main, style="Header.TFrame")
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)

        ttk.Label(header, text="ISO Integrity Check", style="Title.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Button(header, text="About", command=self.show_about, style="Tool.TButton").grid(row=0, column=1, sticky="ne")
        ttk.Label(
            header,
            text="Verify ISO downloads with SHA512, SHA256, SHA1, or MD5 checksums.",
            style="Subtitle.TLabel",
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))

        file_frame = ttk.LabelFrame(main, text="ISO file", padding=14, style="Card.TLabelframe")
        file_frame.grid(row=1, column=0, sticky="ew", pady=(22, 0))
        file_frame.columnconfigure(0, weight=1)

        self.file_entry = ttk.Entry(file_frame, textvariable=self.file_path)
        self.file_entry.grid(row=0, column=0, sticky="ew", padx=(0, 10), ipady=4)
        self.attach_context_menu(self.file_entry, readonly=False)

        ttk.Button(file_frame, text="Browse...", command=self.browse_file, style="Tool.TButton").grid(row=0, column=1)

        input_frame = ttk.LabelFrame(main, text="Verification input", padding=14, style="Card.TLabelframe")
        input_frame.grid(row=2, column=0, sticky="ew", pady=(14, 0))
        input_frame.columnconfigure(1, weight=1)

        ttk.Label(input_frame, text="Hash type", style="CardText.TLabel").grid(row=0, column=0, sticky="w", padx=(0, 10))
        self.hash_menu = ttk.Combobox(
            input_frame,
            textvariable=self.algorithm,
            values=list(SUPPORTED_HASHES.keys()),
            state="readonly",
            width=12,
        )
        self.hash_menu.grid(row=0, column=1, sticky="w")

        ttk.Button(
            input_frame,
            text="Import checksum file...",
            command=self.browse_checksum_file,
            style="Tool.TButton",
        ).grid(row=0, column=2, sticky="e")

        ttk.Label(input_frame, text="Expected checksum", style="CardText.TLabel").grid(
            row=1, column=0, sticky="w", padx=(0, 10), pady=(14, 0)
        )
        self.expected_text = ttk.Entry(input_frame)
        self.expected_text.grid(row=1, column=1, columnspan=2, sticky="ew", pady=(14, 0), ipady=4)
        self.attach_context_menu(self.expected_text, readonly=False)

        action_frame = ttk.Frame(main, style="App.TFrame")
        action_frame.grid(row=3, column=0, sticky="ew", pady=(18, 0))
        action_frame.columnconfigure(0, weight=1)

        self.progress = ttk.Progressbar(action_frame, mode="indeterminate")
        self.progress.grid(row=0, column=0, sticky="ew", padx=(0, 14))

        self.verify_button = ttk.Button(
            action_frame,
            text="Calculate / Verify",
            command=self.start_verification,
            style="Primary.TButton",
        )
        self.verify_button.grid(row=0, column=1, sticky="e")

        result_frame = ttk.LabelFrame(main, text="Result", padding=14, style="Card.TLabelframe")
        result_frame.grid(row=4, column=0, sticky="ew", pady=(14, 0))
        result_frame.columnconfigure(0, weight=1)

        self.result_label = ttk.Label(result_frame, textvariable=self.status_text, style="Status.TLabel")
        self.result_label.grid(row=0, column=0, sticky="w")
        self.detail_label = ttk.Label(result_frame, textvariable=self.detail_text, style="Detail.TLabel")
        self.detail_label.grid(row=1, column=0, sticky="w", pady=(4, 0))

        computed_frame = ttk.LabelFrame(main, text="Computed checksum", padding=14, style="Card.TLabelframe")
        computed_frame.grid(row=5, column=0, sticky="nsew", pady=(14, 0))
        computed_frame.columnconfigure(0, weight=1)
        computed_frame.rowconfigure(0, weight=1)

        self.computed_text = ttk.Entry(computed_frame, state="readonly")
        self.computed_text.grid(row=0, column=0, sticky="ew", ipady=4)
        self.attach_context_menu(self.computed_text, readonly=True)

        button_row = ttk.Frame(computed_frame)
        button_row.grid(row=1, column=0, sticky="ew", pady=(12, 0))
        button_row.columnconfigure(0, weight=1)

        ttk.Button(
            button_row,
            text="Copy computed checksum",
            command=self.copy_computed_hash,
            style="Tool.TButton",
        ).grid(row=0, column=1, sticky="e")

        ttk.Label(
            main,
            text="Only trust checksums from the official operating system or vendor download page. SHA1 and MD5 are legacy options.",
            style="Muted.TLabel",
        ).grid(row=6, column=0, sticky="w", pady=(14, 0))

    def attach_context_menu(self, widget: ttk.Entry, readonly: bool) -> None:
        menu = Menu(widget, tearoff=False)
        if not readonly:
            menu.add_command(label="Cut", command=lambda: widget.event_generate("<<Cut>>"))
        menu.add_command(label="Copy", command=lambda: widget.event_generate("<<Copy>>"))
        if not readonly:
            menu.add_command(label="Paste", command=lambda: widget.event_generate("<<Paste>>"))
        menu.add_separator()
        menu.add_command(label="Select All", command=lambda: self.select_all(widget))

        def show_menu(event) -> None:
            menu.tk_popup(event.x_root, event.y_root)
            menu.grab_release()

        widget.bind("<Button-3>", show_menu)
        widget.bind("<Control-a>", lambda event: self.select_all(widget))

    def select_all(self, widget: ttk.Entry):
        widget.selection_range(0, END)
        widget.icursor(INSERT)
        return "break"

    def browse_file(self) -> None:
        selected = filedialog.askopenfilename(
            title="Choose ISO file",
            filetypes=(("ISO files", "*.iso"), ("All files", "*.*")),
        )
        if selected:
            self.file_path.set(selected)
            self.detail_text.set("ISO selected. Paste or import the matching checksum.")

    def browse_checksum_file(self) -> None:
        selected = filedialog.askopenfilename(
            title="Choose checksum file",
            filetypes=(
                ("Checksum files", "*.sha256 *.sha512 *.sha1 *.md5 *.txt *SUMS"),
                ("All files", "*.*"),
            ),
        )
        if not selected:
            return

        try:
            parsed = load_checksum_file(selected, iso_path=self.file_path.get() or None)
        except ValueError as exc:
            messagebox.showerror("Checksum file", str(exc))
            self.set_status("error", "Checksum file could not be imported.", str(exc))
            return

        self.algorithm.set(parsed.algorithm)
        self.expected_text.delete(0, END)
        self.expected_text.insert(0, parsed.checksum)

        source = Path(selected).name
        if parsed.filename:
            detail = f"Imported {parsed.algorithm} from {source}, line {parsed.line_number}: {parsed.filename}"
        else:
            detail = f"Imported {parsed.algorithm} from {source}, line {parsed.line_number}."
        self.set_status("ready", "Checksum imported.", detail)

    def start_verification(self) -> None:
        self.set_running(True)
        self.set_status("ready", "Reading file and calculating checksum...", "Large ISO files can take a little while.")
        self.set_computed_hash("")

        file_path = self.file_path.get()
        expected_checksum = self.expected_text.get()
        algorithm = self.algorithm.get()

        worker = threading.Thread(
            target=self._run_verification,
            args=(file_path, expected_checksum, algorithm),
            daemon=True,
        )
        worker.start()
        self.root.after(100, self.poll_worker)

    def _run_verification(self, file_path: str, expected_checksum: str, algorithm: str) -> None:
        try:
            self.worker_messages.put(verify_checksum(file_path, expected_checksum, algorithm))
        except Exception as exc:
            self.worker_messages.put(exc)

    def poll_worker(self) -> None:
        try:
            message = self.worker_messages.get_nowait()
        except queue.Empty:
            self.root.after(100, self.poll_worker)
            return

        self.set_running(False)
        if isinstance(message, Exception):
            self.set_status("error", f"Error: {message}", "The checksum could not be calculated.")
            return

        detail = self._result_detail(message)
        self.set_status(message.status, message.message, detail)
        self.set_computed_hash(message.computed_hash)

    def set_running(self, running: bool) -> None:
        self.verify_button.configure(state=DISABLED if running else NORMAL)
        if running:
            self.progress.start(10)
        else:
            self.progress.stop()

    def set_status(self, status: str, message: str, detail: str = "") -> None:
        colors = {
            "match": "#107c41",
            "mismatch": "#b3261e",
            "error": "#b3261e",
            "generated": "#2454a6",
            "ready": "#243044",
        }
        self.status_text.set(message)
        self.detail_text.set(detail)
        self.result_label.configure(foreground=colors.get(status, "#243044"))

    def set_computed_hash(self, value: str) -> None:
        self.computed_text.configure(state=NORMAL)
        self.computed_text.delete(0, END)
        self.computed_text.insert(0, value)
        self.computed_text.configure(state="readonly")

    def copy_computed_hash(self) -> None:
        value = self.computed_text.get()
        if not value:
            messagebox.showinfo("No checksum", "Calculate a checksum first.")
            return

        self.root.clipboard_clear()
        self.root.clipboard_append(value)
        self.set_status("ready", "Computed checksum copied to the clipboard.", "")

    def show_about(self) -> None:
        messagebox.showinfo(
            "About ISO Integrity Check",
            "ISO Integrity Check\n\n"
            f"Created by {APP_AUTHOR}\n"
            f"{APP_PROFILE_URL}\n\n"
            "Verify ISO downloads with SHA512, SHA256, SHA1, and MD5 checksums.\n\n"
            "SHA1 and MD5 are legacy options. Prefer SHA256 or SHA512 from an official source.",
        )

    def _result_detail(self, result: VerificationResult) -> str:
        algorithm = self.algorithm.get()
        if result.status == "generated":
            return f"Computed {algorithm}. Import or paste an official checksum to compare."
        if result.status == "match":
            return f"Computed {algorithm} equals the expected checksum."
        if result.status == "mismatch":
            return f"Computed {algorithm} differs from the expected checksum."
        return ""


def main() -> None:
    root = Tk()
    ISOIntegrityApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
