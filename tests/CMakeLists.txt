set( LDC2_BIN        ${PROJECT_BINARY_DIR}/bin/${LDC_EXE}  )
set( LLVM_TOOLS_DIR  ${LLVM_ROOT_DIR}/bin                  )
set( LDC2_BIN_DIR    ${PROJECT_BINARY_DIR}/bin             )
set( TESTS_IR_DIR    ${CMAKE_CURRENT_SOURCE_DIR}           )

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set( DEFAULT_TARGET_BITS 64 )
else()
    set( DEFAULT_TARGET_BITS 32 )
endif()

configure_file(lit.site.cfg.in lit.site.cfg )
configure_file(runlit.py       runlit.py    COPYONLY)

add_test(NAME lit-tests
    COMMAND python runlit.py -v .
)

