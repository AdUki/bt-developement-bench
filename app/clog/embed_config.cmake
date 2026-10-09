# clog.conf as a C++ string (clog's CONFIG_STRING_VAR), for the compiled-in configuration.
file(READ ${IN} conf)
file(WRITE ${OUT} "#include <string>\nstd::string ClogCompiledInConfig = R\"CLOGCONF(${conf})CLOGCONF\";\n")
