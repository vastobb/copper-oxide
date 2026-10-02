#pragma once

#include <jni.h>
#include <memory>

namespace copper {

class RendererBase;
class RendererConfig;

bool initializeJNI(JavaVM* vm);
void shutdownJNI();

RendererBase* createRenderer(const RendererConfig& config);
void destroyRenderer(RendererBase* renderer);

} // namespace copper