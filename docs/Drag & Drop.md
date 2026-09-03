# Drag & Drop

Drag & drop is a first-class interaction model in Automat. Automat's ultimate goal is full support of input & output through DnD, including all of the quality-of-like DnD extensions - optional metadata, feedback mechanisms, formats, etc.

This document describes the intended design of DnD support in Automat.

## X11

When a file is being dragged over Automat, it immediately creates a "DataOffer" object & places it under the mouse position. The goal of this object is to receive & describe the data transferred from another application. DataOffer starts data transfer immediately & uses proxy images to describe its contents even before they're fully received.

DataOffer uses regular DragLocationAction for dragging.

On X11 DataOffer stores the result as a new file in the same directory as 'automat_state.json'.

If XdndActionCopy then DataOffer creates the new file directly. When XdndActionMove is used, DataOffer is expected to locate the file(s) using the "text/uri-list" type & move it into Automat's directory. XdndActionMove is refused if copy-free move is not possible (fallback to regular Copy).

DataOffer uses the extension from XdndDirectSave0 (or from "text/uri-list") to display the icon of the file type being transferred. Single DataOffer handles multiple files being transferred. It displays a ring around the icon indicating the transfer progress.

If a file with the given name already exists, Automat checks if the base name ends with a number and bumps the number by one - until a the given filename is available. If there is no number then Automat adds "2" (the file without number is assumed to have number 1).

Once the transfer completes (which typically happens while drag is still in progress), DataOffer identifies the file format and creates another object - File. The newly created object is created in the same place as DataOffer - dragged if DataOffer was dragged - and on the Board if it was dropped on the Board.

Once all transfers complete, DataOffer deletes itself (leaving only the transferred Files).

If the drag ends without a drop, the Files are deleted.

## File

File is an Object that wraps arbitrary files. It recognizes image formats, loads them and displays them directly. Other file types are displayed as icons.

File object "owns" the OS file - when the object is deleted by the user, the OS file is removed as well. This does not happen when Automat is being closed. File serializes the managed path as a "file://" URI.

## Core use-cases to test

- dragging in images from a file browser
- dragging in images from Chormium
- dragging in images from Firefox

# TODOs (future)

- Win32 support
- non-image files
- dragging out of Automat