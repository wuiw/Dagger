configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg
  )
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.in
  ${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg
  )

# Don't include check-llvm into check-all without LLVM_BUILD_TOOLS.
if(NOT LLVM_BUILD_TOOLS)
  set(EXCLUDE_FROM_ALL ON)
endif()

# Set the depends list as a variable so that it can grow conditionally.
# NOTE: Sync the substitutions in test/lit.cfg when adding to this list.
set(LLVM_TEST_DEPENDS
          llvm-config
          UnitTests
          BugpointPasses
          LLVMHello
          bugpoint
          llc
          lli
          lli-child-target
          llvm-ar
          llvm-as
          llvm-bcanalyzer
          llvm-c-test
          llvm-cov
          llvm-cxxdump
          llvm-diff
          llvm-dis
          llvm-dsymutil
          llvm-dwarfdump
          llvm-extract
          llvm-link
          llvm-lto
          llvm-mc
          llvm-mcmarkup
          llvm-nm
          llvm-objdump
          llvm-profdata
          llvm-ranlib
          llvm-readobj
          llvm-rtdyld
          llvm-size
          llvm-symbolizer
          llvm-tblgen
          macho-dump
          opt
          FileCheck
          count
          not
          yaml2obj
          obj2yaml
          llvm-dc
          verify-uselistorder
        )

# If Intel JIT events are supported, depend on a tool that tests the listener.
if( LLVM_USE_INTEL_JITEVENTS )
  set(LLVM_TEST_DEPENDS ${LLVM_TEST_DEPENDS} llvm-jitlistener)
endif( LLVM_USE_INTEL_JITEVENTS )

if(TARGET LLVMgold)
  set(LLVM_TEST_DEPENDS ${LLVM_TEST_DEPENDS} LLVMgold)
endif()

if(TARGET llvm-go)
  set(LLVM_TEST_DEPENDS ${LLVM_TEST_DEPENDS} llvm-go)
endif()

if(APPLE)
  set(LLVM_TEST_DEPENDS ${LLVM_TEST_DEPENDS} LTO)
endif()

if(TARGET ocaml_llvm)
  set(LLVM_TEST_DEPENDS ${LLVM_TEST_DEPENDS}
          ocaml_llvm
          ocaml_llvm_all_backends
          ocaml_llvm_analysis
          ocaml_llvm_bitreader
          ocaml_llvm_bitwriter
          ocaml_llvm_executionengine
          ocaml_llvm_irreader
          ocaml_llvm_linker
          ocaml_llvm_target
          ocaml_llvm_ipo
          ocaml_llvm_passmgr_builder
          ocaml_llvm_scalar_opts
          ocaml_llvm_transform_utils
          ocaml_llvm_vectorize
        )
endif()

add_lit_testsuite(check-llvm "Running the LLVM regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  PARAMS llvm_site_config=${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg
         llvm_unit_site_config=${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg
  DEPENDS ${LLVM_TEST_DEPENDS}
  )
set_target_properties(check-llvm PROPERTIES FOLDER "Tests")

add_lit_testsuites(LLVM ${CMAKE_CURRENT_SOURCE_DIR}
  PARAMS llvm_site_config=${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg
         llvm_unit_site_config=${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg
  DEPENDS ${LLVM_TEST_DEPENDS}
  )

# Setup a legacy alias for 'check-llvm'. This will likely change to be an
# alias for 'check-all' at some point in the future.
add_custom_target(check)
add_dependencies(check check-llvm)
set_target_properties(check PROPERTIES FOLDER "Tests")
