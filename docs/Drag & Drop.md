# Drag & Drop

Drag & drop is a first-class interaction model in Automat. Automat's ultimate goal is full support of input & output through DnD, including all of the quality-of-like DnD extensions - optional metadata, feedback mechanisms, formats, etc.

This document describes the intended design of DnD support in Automat.

## X11

When a file is being dragged over Automat, it should immediately create a "DataOffer" object & place it under the mouse position. The goal of this object is to receive & describe the data transferred from another application. DataOffer should start data transfer immediately & use some proxy image to describe its contents even before they're fully received.

DataOffer should use regular DragLocationAction for dragging.

On X11 DataOffer should use XDS (extension of XDND) to store the result as a new file in the same directory as 'automat_state.json'. If XDS fails - it should fall back to the "application/octet-stream" type.

If XdndActionCopy then DataOffer should create the new file directly. With XdndActionDirectSave the remote app is expected to create the file. When XdndActionMove is used, DataOffer is expected to locate the file(s) using the "text/uri-list" type & move it into Automat's directory. XdndActionMove should be refused if copy-free move is not possible. XdndActionAsk should be replaced by XdndActionCopy.

DataOffer should use the extension from XdndDirectSave0 (or from "text/uri-list") to display the icon of the file type being transferred. Single DataOffer should be able to handle multiple files being transferred. Around the icon it should display a ring indicating the transfer progress.

If a file with the given name already exists, Automat should check if the base name ends with a number and bump the number by one - until a the given filename is available. If there is no number then Automat should add "2" (the file without number is assumed to have 1).

Once the transfer completes, DataOffer should identify the file format and create another object - File. The newly created object should be created in the same place as DataOffer - it should be dragged if DataOffer was dragged - and it should be part of the Board if it was dropped on the Board.

Once all transfers complete, DataOffer should delete itself (leaving only the transferred Files).

If the drag ends without a drop, the Files should be deleted.

## File

File is an Object that wraps arbitrary files. It should recognize image formats, load them and display them directly. Other file types should be displayed as icons.

File object "owns" the OS file - if the object is deleted by the user, the OS file should be removed as well. This should not happen when Automat is closed. File should serialize the managed path as a "file://" URI.

File URIs should include the hostname: "file://hostname/home/user/image.png".

## Core use-cases to test

- dragging in images from a file browser
- dragging in images from Chormium
- dragging in images from Firefox

# TODOs (future)

- Win32 support
- non-image files
- dragging out of Automat