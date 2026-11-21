# Header-only Boost from BOOST_INCLUDE_DIR: libtorrent 2.0 needs no compiled Boost libraries
if (NOT TARGET Boost::headers)
    add_library(Boost::headers INTERFACE IMPORTED)
    set_target_properties(Boost::headers PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${P2P_BOOST_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "BOOST_ALL_NO_LIB")
endif ()

file(STRINGS "${P2P_BOOST_INCLUDE_DIR}/boost/version.hpp" _p2p_boost_ver REGEX "^#define BOOST_VERSION [0-9]+")
string(REGEX REPLACE ".* ([0-9]+)" "\\1" _p2p_boost_ver "${_p2p_boost_ver}")
math(EXPR Boost_MAJOR_VERSION "${_p2p_boost_ver} / 100000")
math(EXPR Boost_MINOR_VERSION "${_p2p_boost_ver} / 100 % 1000")
math(EXPR Boost_SUBMINOR_VERSION "${_p2p_boost_ver} % 100")
set(Boost_VERSION "${Boost_MAJOR_VERSION}.${Boost_MINOR_VERSION}.${Boost_SUBMINOR_VERSION}")
set(Boost_VERSION_STRING "${Boost_VERSION}")
set(Boost_INCLUDE_DIRS "${P2P_BOOST_INCLUDE_DIR}")
set(Boost_FOUND TRUE)
