#ifndef CONFIG_FEATURES_H_
#define CONFIG_FEATURES_H_

#cmakedefine KAMD_DATA_DIR "@KAMD_DATA_DIR@"

#cmakedefine KAMD_PLUGIN_DIR "@KAMD_PLUGIN_DIR@"
#cmakedefine KAMD_FULL_PLUGIN_DIR "@KAMD_FULL_PLUGIN_DIR@"

#cmakedefine KAMD_FULL_BIN_DIR "@KAMD_FULL_BIN_DIR@"

#cmakedefine01 HAVE_CXX11_AUTO
#cmakedefine01 HAVE_CXX11_NULLPTR
#cmakedefine01 HAVE_CXX11_LAMBDA
#cmakedefine01 HAVE_CXX11_OVERRIDE
#cmakedefine01 HAVE_CXX_OVERRIDE_ATTR

#endif
