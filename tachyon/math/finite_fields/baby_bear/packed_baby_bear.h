#ifndef TACHYON_MATH_FINITE_FIELDS_BABY_BEAR_PACKED_BABY_BEAR_H_
#define TACHYON_MATH_FINITE_FIELDS_BABY_BEAR_PACKED_BABY_BEAR_H_

#include "tachyon/build/build_config.h"

#if ARCH_CPU_X86_64
#if defined(TACHYON_HAS_AVX512)
#include "tachyon/math/finite_fields/baby_bear/packed_baby_bear_avx512.h"
#else
#include "tachyon/math/finite_fields/baby_bear/packed_baby_bear_avx2.h"
#endif
#elif ARCH_CPU_ARM64
#include "tachyon/math/finite_fields/baby_bear/packed_baby_bear_neon.h"
#endif
#endif

namespace tachyon::math {

#if ARCH_CPU_X86_64
#if defined(TACHYON_HAS_AVX512)
using PackedBabyBear = PackedBabyBearAVX512;
#else
using PackedBabyBear = PackedBabyBearAVX2;
#endif
#elif ARCH_CPU_ARM64
using PackedBabyBear = PackedBabyBearNeon;
#endif

}  // namespace tachyon::math

#endif  // TACHYON_MATH_FINITE_FIELDS_BABY_BEAR_PACKED_BABY_BEAR_H_
