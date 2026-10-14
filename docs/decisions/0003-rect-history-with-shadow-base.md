# 0003 Rect-based undo with a synchronized shadow copy
Status: accepted
Copying the full picture at the start of every edit was slow on large images. `EaselSyncBuf` keeps a full copy
current by copying only the rectangles marked stale; an edit's "before" is that copy and the history stores only
the changed bounding box. Alternative considered: tile copy-on-write — more complex, no measured need.
