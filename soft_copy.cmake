# Copies SRC into the folder DST without failing the build when the target file is in use
# (for example KuangOptimizer.exe running from dist\). Just prints a warning instead.
execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory "${DST}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC}" "${DST}" RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0)
    get_filename_component(name "${SRC}" NAME)
    message(WARNING "No se pudo copiar ${name} a ${DST} (esta abierto). Cierra el programa o usa KuangInstaller.exe.")
endif()
