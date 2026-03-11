#pragma once
#include "../Activity.h"

class BootActivity final : public Activity {
  const char* subtitle;
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* subtitle = nullptr)
      : Activity("Boot", renderer, mappedInput), subtitle(subtitle) {}
  void onEnter() override;
};
