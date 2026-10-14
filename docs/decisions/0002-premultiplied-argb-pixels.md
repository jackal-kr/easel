# 0002 Pixels are premultiplied 0xAARRGGBB in one buffer
Status: accepted
Matches Cairo ARGB32 and GDK_MEMORY_B8G8R8A8_PREMULTIPLIED on little-endian, so texture upload is a copy.
Straight-alpha conversion happens only at file I/O. Hard-edged tools mean most pixels are opaque anyway.
