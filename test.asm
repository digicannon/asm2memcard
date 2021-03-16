# ====================
#  Insert at 8025C00C
# ====================

    # Load address of mode text.
    subi r3, r3, 40
    lis r14, 0x8045
    ori r14, r14, 0xBF14
    lbz r14, 0(r14)
    cmpli 0, r14, 1
    bne store
    subi r3, r3, 2
store:
    bl load_text
    mflr r4
    li r5, 24
    # Call memcpy.
    lis r14, 0x8000
    ori r14, r14, 0x31F4
    mtctr r14
    bctrl
    b return
load_text:
    blrl
    .long 0x1A1A1A1A
    .long 0x20192005
    .long 0x1A202420
    .long 0x3120271A
    .long 0x20192006
    .long 0

return:
	lwz r0, 0x0014(sp)
