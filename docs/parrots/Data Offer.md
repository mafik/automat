# Data Offer

The Data Offer is the object that receives data dragged into Automat from another program. It is created when a drag enters the window, shows one card per file with a progress ring, replaces each finished transfer with a `File` object (`src/library_file.hpp`), and removes itself when every transfer has finished. The card is dragged by the regular `DragLocationAction`, so the preview the user sees during the drag is the same as any other drag (`docs/parrots/Boards.md`, "Dragging"). A single image whose transfer finished before the drop onto the starfield becomes the wallpaper of the new board (`docs/parrots/Boards.md`, "Wallpaper"); a transfer still running at the drop places its File on the new board as an object when it arrives.

The object is defined in `src/library_data_offer.hpp` and `src/library_data_offer.cpp`. The platform frontends only translate protocol events into calls on it: `src/xcb_window.cpp` handles the XDND client messages and the X selection transfers, `src/win32_window.cpp` implements an OLE `IDropTarget`. Both call `DataOffer::BeginDragAction` when the drag enters, start the transfers (`DataOffer::AddPath` and `DataOffer::Add` on X11, `DataOffer::Receive` on Windows), and call `DataOffer::CancelDragAction` when the drag leaves and `DataOffer::EndDragAction` when it drops. These three functions manage the drag of the card, not the transfers: a transfer usually finishes while the card is still dragged, but it may also continue after the card has been dropped onto a board.

## Why the transfers are tasks

Every call that can wait for another process is a blocking call: reading a file named by the source, writing the entry's File to disk, and on Windows every `IDataObject::GetData` and `IStream::Read`, because the source produces the data inside the call (Firefox downloads the image inside it, Explorer extracts a zip entry inside it). The thread that delivers input to the UI must not wait for that, otherwise the card stops following the pointer. So every transfer runs as a `FunctionTask` (`src/tasks.hpp`) on the worker threads, with the offer as the task target. A task that finds its offer gone, or finds `cancelled` set, does nothing.

The main thread keeps the operations that answer at once: the protocol replies, the format queries (`EnumFormatEtc`/`QueryGetData` on Windows, the property reads on X11), the pointer updates, the accumulation of an X11 octet stream (the source paces it, the receiver only copies chunks out of its window properties), and everything that touches widgets.

One `FunctionTask` is scheduled per entry. The lambda captures the entry by reference, which is safe because the entries live inside the offer and the task keeps the offer alive while it runs. The X11 move deletion task captures the offer itself, because it is scheduled when the card may already be gone and nothing else would keep the offer alive until it runs.

## Completion and the main-loop hop

A transfer ends by calling `DataOffer::EntryWriteOver`, which writes the entry's File to its disk file (`File::FlushToFilesystem`), marks the entry done, reports a failure as the offer's Error and posts `DataOffer::TryFinishDrop` to the main loop through `ui::Window::RunOnMainLoop` (`src/window.hpp`). On Windows the post is a `WM_USER` message; on X11 it is a client message of type `AUTOMAT_POSTED_FUNCTION` that the window sends to itself with the function pointer in the message data (`XCBWindow::RunOnMainLoop`). A post made on the main thread runs the function at once.

`TryFinishDrop` runs on the main thread under the window lock. It takes the `File` out of every finished entry that still holds one, analyzes its contents with `File::Analyze` (the image, the icon and the size, which must be computed on the main thread), creates a `Location` for it, adds the Location to the drag group while the card is still dragged (`DragLocationAction::AddToGroup`) or inserts it into the card's board after the drop, and when `DataOffer::Done` holds it removes the card and sends the platform reply. The function is idempotent, so the order of posts does not matter and `EndDragAction` can call it directly.

`Done` requires `sealed`, which the frontend or the describe task sets after the last entry has been added. On Windows the entries are added on a worker, so the describe task posts `TryFinishDrop` once more after sealing; an entry that completed before the seal would otherwise never trigger the final removal.

## Destinations

`DataOffer::Add` records the entry's name and creates the entry's `File` (`src/library_file.hpp`) with that name. The File's constructor creates its disk file inside `AutomatDir()` with `CreateUniqueFile` (`src/file_import.cpp`), which appends a number to the name until an exclusive create succeeds, so a File never takes over a file that already exists. The path does not change afterwards. The transfer appends the bytes to the File with `File::Append`, and `EntryWriteOver` writes them to the disk file once the transfer is complete.

A `File` owns its disk file and deletes it with the object. An entry whose File is never spawned, because its transfer was cancelled, failed or was still running at exit, therefore leaves nothing on disk once the offer releases the File.

## Cancellation and failure

`CancelDragAction` sets `cancelled` and discards the whole drag group, which destroys the card, every `File` spawned so far and their files. A running file read stops between 64 KiB slices and a running stream read stops between chunks; queued tasks find the flag set and return before reading anything. An entry whose `File` was not spawned needs no cleanup code: the File is released with the offer and removes its disk file.

A failed entry keeps its card: `EntryWriteOver` reports the error on the offer, and `TryFinishDrop` skips the card removal while the offer carries an Error, so the card stays on the board with the message until the user deletes it. The platform reply is still sent after a drop, with the drop reported as accepted when at least one entry succeeded (`DataOffer::Accepted`).

A saved state that contains such a card restores an empty Data Offer with an indeterminate ring, because the Error is not serialized.

## X11

A `text/uri-list` offer is probed once; every `file://` entry becomes `DataOffer::AddPath`, which schedules a task that maps the named file with `fs::real.Map` and appends it to the entry's File in 64 KiB slices, so the ring shows the progress and a cancellation stops between slices. An `application/octet-stream` offer arrives through the X selection, in one property or as `INCR` chunks; the event loop appends each chunk to the entry's File and schedules a task that calls `EntryWriteOver` when the last chunk has arrived. The X11 state of the transfer (source window, target window, requested action, offered types, the `XdndDirectSave0` name, the per-entry selection matching keys) stays in the offer under `#if defined(__linux__)`.

`XdndFinished` is sent from `TryFinishDrop` when the offer is done after a drop; the `source` field is cleared afterwards so that a late post does not send it twice. For `XdndActionMove` the reply follows the deletion of the source files, which runs in a task.

File URIs are written as `file:///path` without a hostname and parsed in that form (`FileURIFromPath`, `PathFromFileURI`); on Windows the form is `file:///C:/path`.

## Windows

`Win32Window::Make` initialises OLE on the main thread and registers a file-scope `OleDropTarget` with `RegisterDragDrop`; the window's destructor revokes it. `DragEnter` hands the data object to `DataOffer::Receive`, which chooses the transfer model from the formats the source offers, in the order `CF_HDROP`, `FileGroupDescriptorW`, `PNG`, `CF_DIBV5`, `CF_DIB`, and reports that none is usable, in which case the drag is refused. The drop effect is always a copy; no modifier key is interpreted. The shell drag image is not enabled (`IDropTargetHelper` is not used), so the card is the only representation of the transfer.

The data object received by `DragEnter` belongs to the main thread's single-threaded apartment. The offer registers it in the Global Interface Table and keeps the cookie; every task fetches its own proxy from the table (`GetInterfaceFromGlobal`) and releases it when it is done. The worker threads join the multithreaded apartment in `AutomatLoop` (`src/tasks.cpp`) for that purpose. The table entry is revoked, and an asynchronous operation ended, by a function posted to the main loop, because those calls must come from the apartment that registered the object; `ReleaseSource` posts it from `CancelDragAction`, from the final `TryFinishDrop` and from the destructor.

`Receive` schedules one describe task whose body depends on the model: for `CF_HDROP` it reads the path list and calls `DataOffer::AddPath` per path, as on X11, for `FileGroupDescriptorW` it reads the descriptor list and schedules a read task per item with `FileContents` and the item index, and for a bitmap format it adds one entry named `image.png` or `image.bmp` and schedules a read task for the format. A read task accepts an `HGLOBAL` block or an `IStream`; a stream is read in 64 KiB chunks straight into the entry's File with the size taken from `IStream::Stat` when the descriptor had none, and a DIB block gets a `BITMAPFILEHEADER` in front so that Skia's BMP decoder reads it.

When the drop happens before every transfer has finished and the source supports `IDataObjectAsyncCapability`, `EndDragAction` calls `StartOperation`, so the source's `DoDragDrop` returns while the tasks keep reading; `EndOperation` follows the last completion.

## Not covered

Drags that offer only a URL, only `CF_BITMAP`, or only storage mediums other than a block or a stream are refused or fail with an Error. Move semantics exist only on X11. Directories in a dropped selection fail as entries on Windows and on X11.
