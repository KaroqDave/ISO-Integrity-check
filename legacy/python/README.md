# Legacy Python GUI

This folder contains the original Tkinter implementation of ISO Integrity Check. It is preserved for reference and compatibility, but the C++/Qt app at the repository root is now the primary version.

Run the legacy GUI:

```powershell
python .\legacy\python\main.py
```

Run the legacy Python and PowerShell compatibility tests:

```powershell
python -m unittest discover -s .\legacy\python
```

The legacy implementation uses only Python's standard library.
