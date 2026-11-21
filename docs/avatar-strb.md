# Reading STRB blocks

The bounded reader locates a block inside a caller-supplied STRB buffer. It
borrows the block without allocating a copy. STRB is a container: successfully
finding a block is not proof of its content type or asset identity. A YTGR
prefix is treated as opaque, not authenticated. No resource data is bundled.

The format references are the public
[Gibbed STRB reader](https://github.com/gibbed/xenia/blob/bc08cec2bab5784a75b7f15c04f33dfacb74ffd2/src/xenia/avatars/strb.cc)
and [Velocity reader](https://github.com/hetelek/Velocity/blob/cf0b84cc8bbfad09c655476c6a3c762836ce1246/XboxInternals/AvatarAsset/AvatarAsset.cpp).
They agree on stepping to the next block by aligned byte length. For a
multi-byte element, the Gibbed reader multiplies length by element size when
returning a match; Velocity reads length bytes, treating element size as a
divisor of length. This implementation follows the latter interpretation.
It rejects zero-length blocks, zero element sizes, non-integral element
counts and incomplete headers or payloads. These are validation choices, not
claims about every input accepted by a console.

The tests construct every byte locally. They cover both byte orders,
independent field widths of 1/2/4/8, an unaligned final payload, a prefixed
container, and multiple blocks with non-unit element sizes. They do not
exercise a console, games, real avatar assets, a catalog, rendering or XAM.
