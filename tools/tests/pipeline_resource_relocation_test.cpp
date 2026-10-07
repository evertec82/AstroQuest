// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/info.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
int main() {
  auto source = std::make_unique<Shader::InfoPersistent>();
  for (unsigned i = 0; i < Shader::NUM_IMAGES; ++i)
    source->images.push_back(Shader::ImageResource{.sharp_idx = i + 17});
  for (unsigned i = 0; i < Shader::NUM_SAMPLERS; ++i)
    source->samplers.push_back(Shader::SamplerResource{.sharp_idx = i + 91});
  std::vector<unsigned char> bytes(sizeof(*source));
  std::memcpy(bytes.data(), source.get(), bytes.size());
  auto restored = std::make_unique<Shader::InfoPersistent>();
  std::memcpy(restored.get(), bytes.data(), bytes.size());
  source.reset(); // Original process/object storage is gone, as on a warm cache
                  // launch.
  int failures = 0;
  auto inside = [&](const void *p) {
    auto at = reinterpret_cast<uintptr_t>(p),
         begin = reinterpret_cast<uintptr_t>(restored.get());
    return at >= begin && at < begin + sizeof(*restored);
  };
  if (!inside(restored->images.data()) || !inside(restored->samplers.data()))
    ++failures;
  if (restored->images.size() != Shader::NUM_IMAGES ||
      restored->samplers.size() != Shader::NUM_SAMPLERS)
    ++failures;
  for (unsigned i = 0; i < Shader::NUM_IMAGES; ++i)
    if (restored->images[i].sharp_idx != i + 17)
      ++failures;
  for (unsigned i = 0; i < Shader::NUM_SAMPLERS; ++i)
    if (restored->samplers[i].sharp_idx != i + 91)
      ++failures;
  printf("persistent shader image/sampler lists survive byte relocation and "
         "original destruction at full capacity: %d failures\n",
         failures);
  return failures ? 1 : 0;
}
