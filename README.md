# CAD Thumbnails for Windows File Explorer

Shows a picture of the part instead of a generic icon in File Explorer for:

| Format | Extensions | How the thumbnail is made |
|---|---|---|
| SolidWorks | `.sldprt` `.sldasm` `.slddrw` | Uses the preview image SolidWorks saves inside the file (old OLE format and the 2015+ format both work). SolidWorks does **not** need to be installed. |
| STEP | `.step` `.stp` | Reads the B-rep geometry, tessellates the faces and draws a shaded isometric view with edges. Assemblies are positioned correctly. |
| STL | `.stl` | Renders the mesh (ASCII or binary) as a shaded isometric view. |

Works on 64-bit Windows 10 and 11. Installs for your user only; no admin rights needed.

## Install

1. Download this repository (green **Code** button → **Download ZIP**) and unzip it.
2. Open the `dist` folder and double-click **`install.bat`**.
   - If Windows SmartScreen warns about the file, choose **More info → Run anyway** (the DLL isn't code-signed).
3. Answer **Y** when asked to restart File Explorer.
4. In Explorer, switch to **View → Medium icons** (or larger).

The DLL is copied to `%LOCALAPPDATA%\CadThumbnails`.

## Uninstall

Double-click `dist\uninstall.bat`.

## Troubleshooting

- **Still seeing plain icons?** Make sure *“Always show icons, never thumbnails”* is unticked in
  *File Explorer Options → View*. Then clear the thumbnail cache: run **Disk Cleanup**, tick
  **Thumbnails**, and restart Explorer (or re-run `install.bat` and answer Y).
- **A SolidWorks file shows no picture**: the file was saved without a preview (rare, e.g. some
  files written by batch tools). Re-saving it in SolidWorks fixes it.
- **Another program took over thumbnails** (e.g. a CAD viewer installed later): run `install.bat` again.

## Build from source

The DLL is plain C++17 with no external libraries. On Linux/WSL with MinGW-w64:

```sh
sudo apt install g++-mingw-w64-x86-64
./build.sh
```

This produces `dist/CadThumbnails.dll` and a host test tool, `build/thumbtest <file> <out-prefix> [size]`,
which writes the thumbnail to an image so the renderer can be checked without Windows.

## Layout

- `src/` – portable core: STL loader, STEP reader/tessellator, SolidWorks preview extractor
  (OLE compound file + 2015+ container with a built-in DEFLATE decoder), software renderer.
- `win/dllmain.cpp` – the Explorer `IThumbnailProvider` COM handler and per-user registration.
- `tools/` – `thumbtest` (host) and `wintest` (Windows: registers the DLL, loads it through COM
  like Explorer does and saves the thumbnail as a BMP).
