#pragma once

#ifdef USE_BOOST_REGEX

#include "regex_boost.h"

#elif USE_QT_REGEX

#include "regex_qt.h"

#else

#include "regex_std.h"

#endif
