#pragma once

// Hand-written ABI mirrors of the few Ogre types we touch. The game's OgreMain.dll was
// built with VS2008; we only rely on layouts that are plain data or vtable order, never
// on STL members.

namespace ogre_abi {

struct FrameEvent
{
  float timeSinceLastEvent;
  float timeSinceLastFrame;
};

// Vtable order verified against CGame's FrameListener sub-vtable (0xA8F5E8):
//   [0] frameStarted  [1] frameRenderingQueued  [2] frameEnded  [3] scalar deleting dtor
class FrameListener
{
public:
  virtual bool frameStarted(const FrameEvent&) { return true; }
  virtual bool frameRenderingQueued(const FrameEvent&) { return true; }
  virtual bool frameEnded(const FrameEvent&) { return true; }
  virtual ~FrameListener() = default;
};

using Root = void;

// OgreMain.dll exports, resolved at runtime by mangled name.
using GetRootFn = Root*(__cdecl*)();
using AddFrameListenerFn = void(__thiscall*)(Root*, FrameListener*);
using RemoveFrameListenerFn = void(__thiscall*)(Root*, FrameListener*);

constexpr const char* kGetRoot = "?getSingletonPtr@Root@Ogre@@SAPAV12@XZ";
constexpr const char* kAddFrameListener = "?addFrameListener@Root@Ogre@@QAEXPAVFrameListener@2@@Z";
constexpr const char* kRemoveFrameListener = "?removeFrameListener@Root@Ogre@@QAEXPAVFrameListener@2@@Z";

} // namespace ogre_abi
