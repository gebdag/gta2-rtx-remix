; The block at the start of this DLL's code that covers base + 0x485E0, where
; plugins written for the original renderer look for a pointer to its state.
; See PlaceGraphicsPointer in plugin_d3d6.cpp, which writes the pointer.
;
; In assembler rather than C++ because the compiler marks a #pragma section as
; data, and the linker then keeps it out of .text in a section of its own. A
; 'CODE' segment named .text$a joins .text, ahead of every .text$mn the
; compiler emits. The size is kReservedBytes in plugin_d3d6.cpp.
.686
.model flat

RESERVED SEGMENT PARA PUBLIC READ EXECUTE ALIAS(".text$a") 'CODE'
PUBLIC _g_reservedForRendererState
_g_reservedForRendererState LABEL BYTE
    BYTE 49000h DUP (0)
RESERVED ENDS

END
