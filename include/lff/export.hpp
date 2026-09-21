// Link Failover Fabric — public symbol export and toolchain surface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#if defined(_WIN32) && defined(LFF_SHARED_BUILD)
#  if defined(LFF_BUILDING_LIBRARY)
#    define LFF_API __declspec(dllexport)
#  else
#    define LFF_API __declspec(dllimport)
#  endif
#else
#  define LFF_API
#endif

#if defined(_MSC_VER)
// The public runtime surface is made of value types built from standard
// containers. They are exported for one configured toolchain at a time; binary
// compatibility across different MSVC toolsets or STL versions is explicitly
// not supported, so the "needs dll-interface" diagnostic is disabled here for
// every public header. Consumers must build against the same toolchain.
#  pragma warning(push)
#  pragma warning(disable : 4251)
#endif
