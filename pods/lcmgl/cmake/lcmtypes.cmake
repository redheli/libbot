# Macros for automatically compiling LCM types into C, Java, and Python
# libraries.
#
# The primary macro is
#     lcmtypes_build()
# 
# It expects that the directory ${PROJECT_SOURCE_DIR}/lcmtypes contains all
# the LCM types used by the system.  The macro generates C, Java, and Python
# bindings, and also defines some CMake variables useful for other parts of the
# build system.
#
# After invoking this macro, the following variables will be set:
#
#   LCMTYPES_INCLUDE_DIRS
#   LCMTYPES_LIBS
#
# These variables should be used with any C/C++ programs that use the LCM types
#
# If Java is enabled, then the following variables will be set:
#
#   LCMTYPES_JAR  -- path to the automatically compiled .jar file of LCM types
#
# TODO Python variables

cmake_minimum_required(VERSION 2.6.0)

# Policy settings to prevent warnings on 2.6 but ensure proper operation on
# 2.4.
if(COMMAND cmake_policy)
    # Logical target names must be globally unique.
    cmake_policy(SET CMP0002 OLD)
    # Libraries linked via full path no longer produce linker search paths.
    cmake_policy(SET CMP0003 OLD)
    # Preprocessor definition values are now escaped automatically.
    cmake_policy(SET CMP0005 OLD)
    if(POLICY CMP0011)
        # Included scripts do automatic cmake_policy PUSH and POP.
        cmake_policy(SET CMP0011 OLD)
    endif(POLICY CMP0011)
endif()

macro(lcmtypes_get_types msgvar)
    # get a list of all LCM types
    file(GLOB __tmplcmtypes "${PROJECT_SOURCE_DIR}/lcmtypes/*.lcm")
    set(${msgvar} "")
    foreach(_msg ${__tmplcmtypes})
        # Try to filter out temporary and backup files
        if(${_msg} MATCHES "^[^\\.].*\\.lcm$")
            list(APPEND ${msgvar} ${_msg})
        endif(${_msg} MATCHES "^[^\\.].*\\.lcm$")
    endforeach(_msg)
endmacro()

function(lcmgen)
    execute_process(COMMAND lcm-gen ${ARGV} RESULT_VARIABLE lcmgen_result)
    if(NOT lcmgen_result EQUAL 0)
        message(FATAL_ERROR "lcm-gen failed")
    endif()
endfunction()

function(lcmtypes_add_clean_dir clean_dir)
    get_directory_property(acfiles ADDITIONAL_MAKE_CLEAN_FILES)
    list(APPEND acfiles ${clean_dir})
    set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${acfiles}")
endfunction()

macro(lcmtypes_build_c)
    # generate C bindings for LCM types
    set(_lcmtypes_c_dir ${PROJECT_SOURCE_DIR}/lcmtypes/c/lcmtypes)

    # blow away any existing auto-generated files.
    file(REMOVE_RECURSE ${_lcmtypes_c_dir})

    # run lcm-gen now
    execute_process(COMMAND mkdir -p ${_lcmtypes_c_dir})
    lcmgen(--lazy -c --c-cpath ${_lcmtypes_c_dir} --c-hpath ${_lcmtypes_c_dir} ${_lcmtypes})

    # run lcm-gen at compile time
    add_custom_target(lcmgen_c ALL 
        COMMAND sh -c '[ -d ${_lcmtypes_c_dir} ] || mkdir -p ${_lcmtypes_c_dir}'
        COMMAND sh -c 'lcm-gen --lazy -c ${_lcmtypes} --c-cpath ${_lcmtypes_c_dir} --c-hpath ${_lcmtypes_c_dir}')

    # get a list of all generated .c and .h files
    file(GLOB _lcmtypes_c_files ${_lcmtypes_c_dir}/*.c)
    file(GLOB _lcmtypes_h_files ${_lcmtypes_c_dir}/*.h)

    include_directories(${_lcmtypes_c_dir})

    # aggregate into a static library and a shared library
    ADD_LIBRARY("lcmtypes_${PROJECT_NAME}" SHARED ${_lcmtypes_c_files})
    add_library("lcmtypes_${PROJECT_NAME}-static" STATIC ${_lcmtypes_c_files})
    SET_TARGET_PROPERTIES("lcmtypes_${PROJECT_NAME}-static" PROPERTIES OUTPUT_NAME "lcmtypes_${PROJECT_NAME}")
    SET_TARGET_PROPERTIES("lcmtypes_${PROJECT_NAME}-static" PROPERTIES PREFIX "lib")
    # Help CMake 2.6.x and lower (not necessary for 2.8 and above, but doesn't hurt):
    SET_TARGET_PROPERTIES("lcmtypes_${PROJECT_NAME}" PROPERTIES CLEAN_DIRECT_OUTPUT 1)
    SET_TARGET_PROPERTIES("lcmtypes_${PROJECT_NAME}-static" PROPERTIES CLEAN_DIRECT_OUTPUT 1)

    add_dependencies("lcmtypes_${PROJECT_NAME}" lcmgen_c)
    add_dependencies("lcmtypes_${PROJECT_NAME}-static" lcmgen_c)

    # create a header file aggregating all of the autogenerated .h files
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" __sanitized_project_name "${PROJECT_NAME}")
    set(__agg_h_fname "${_lcmtypes_c_dir}/lcmtypes_${PROJECT_NAME}.h")
    file(WRITE ${__agg_h_fname}
        "#ifndef __lcmtypes_${__sanitized_project_name}_h__\n"
        "#define __lcmtypes_${__sanitized_project_name}_h__\n\n")
    foreach(h_file ${_lcmtypes_h_files})
        file(RELATIVE_PATH __tmp_path ${_lcmtypes_c_dir} ${h_file})
        file(APPEND ${__agg_h_fname} "#include \"${__tmp_path}\"\n")
    endforeach()
    file(APPEND ${__agg_h_fname} "\n#endif\n")
    list(APPEND _lcmtypes_h_files ${__agg_h_fname})
    unset(__sanitized_project_name)
    unset(__agg_h_fname)

    # make header files and libraries public
    install(TARGETS "lcmtypes_${PROJECT_NAME}-static" ARCHIVE DESTINATION lib)
    install(TARGETS "lcmtypes_${PROJECT_NAME}" LIBRARY DESTINATION lib)
    install(FILES ${_lcmtypes_h_files} DESTINATION include/lcmtypes)

    # set some compilation variables
    set(LCMTYPES_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/lcmtypes/c)
    set(LCMTYPES_LIBS "lcmtypes_${PROJECT_NAME}")

    lcmtypes_add_clean_dir("${PROJECT_SOURCE_DIR}/lcmtypes/c")
endmacro()

macro(lcmtypes_build_java)
    # generate Java bindings for LCM types
    set(_lcmtypes_java_dir ${PROJECT_SOURCE_DIR}/lcmtypes/java)

    # blow away any existing auto-generated files.
    file(REMOVE_RECURSE ${_lcmtypes_java_dir})

    # run lcm-gen now
    execute_process(COMMAND mkdir -p ${_lcmtypes_java_dir})
    lcmgen(--lazy -j ${_lcmtypes} --jpath ${_lcmtypes_java_dir})

    # run lcm-gen at compile time
    add_custom_target(lcmgen_java ALL
        COMMAND sh -c '[ -d ${_lcmtypes_java_dir} ] || mkdir -p ${_lcmtypes_java_dir}'
        COMMAND sh -c 'lcm-gen --lazy -j ${_lcmtypes} --jpath ${_lcmtypes_java_dir}')

    # get a list of all generated .java files
    file(GLOB_RECURSE _lcmtypes_java_files ${_lcmtypes_java_dir}/*.java)

    # where is lcm.jar?
    find_package(Java REQUIRED)
    execute_process(COMMAND pkg-config --variable=classpath lcm-java OUTPUT_VARIABLE LCM_JAR_FILE)
    string(STRIP ${LCM_JAR_FILE} LCM_JAR_FILE)
    set(LCMTYPES_JAR ${CMAKE_CURRENT_BINARY_DIR}/lcmtypes_${PROJECT_NAME}.jar)

    # convert the list of .java filenames to a list of .class filenames
    foreach(javafile ${_lcmtypes_java_files})
        string(REPLACE .java .class __tmp_class_fname ${javafile})
        #        add_custom_command(OUTPUT ${__tmp_class_fname} COMMAND
        #            ${JAVA_COMPILE} -source 6 -cp ${_lcmtypes_java_dir}:${lcm_jar} ${javafile} VERBATIM DEPENDS ${javafile})
        list(APPEND _lcmtypes_class_files ${__tmp_class_fname})
        unset(__tmp_class_fname)
    endforeach()

    # add a rule to build the .class files from from the .java files
    add_custom_command(OUTPUT ${_lcmtypes_class_files} COMMAND 
        ${JAVA_COMPILE} -source 6 -cp ${_lcmtypes_java_dir}:${LCM_JAR_FILE} ${_lcmtypes_java_files} 
        DEPENDS ${_lcmtypes_java_files} VERBATIM)

    # add a rule to build a .jar file from the .class files
    add_custom_command(OUTPUT lcmtypes_${PROJECT_NAME}.jar COMMAND
        ${JAVA_ARCHIVE} cf ${LCMTYPES_JAR} -C ${_lcmtypes_java_dir} . DEPENDS ${_lcmtypes_class_files} VERBATIM)
    add_custom_target(lcmtypes_${PROJECT_NAME}_jar ALL DEPENDS ${LCMTYPES_JAR})

    add_dependencies(lcmtypes_${PROJECT_NAME}_jar lcmgen_java)

    install(FILES ${LCMTYPES_JAR} DESTINATION share/java)

    lcmtypes_add_clean_dir(${_lcmtypes_java_dir})
endmacro(lcmtypes_build_java)

macro(lcmtypes_build_python)
    # generate Python bindings for LCM types
    set(_lcmtypes_python_dir ${PROJECT_SOURCE_DIR}/lcmtypes/python)
    execute_process(COMMAND mkdir -p ${_lcmtypes_python_dir})
    execute_process(COMMAND lcm-gen --lazy -p ${_lcmtypes} --ppath ${_lcmtypes_python_dir})

    # run lcm-gen at compile time
    add_custom_target(lcmgen_python ALL
        COMMAND sh -c 'lcm-gen --lazy -p ${_lcmtypes} --ppath ${_lcmtypes_python_dir}')

    # create a distutils setup.py file

    lcmtypes_add_clean_dir(${_lcmtypes_python_dir})
endmacro()

macro(lcmtypes_build)
    lcmtypes_get_types(_lcmtypes)
    list(LENGTH _lcmtypes _num_lcmtypes)
    if(_num_lcmtypes GREATER 0)
        lcmtypes_build_c(_lcmtypes)
        lcmtypes_build_java(_lcmtypes)
        lcmtypes_build_python(_lcmtypes)
    endif()
endmacro()
