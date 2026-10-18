#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Getting the keyboard back. On Linux (X11 / XWayland) a closed popup menu
// can leave no window with the keyboard, and the window manager doesn't
// hand it back on a click: then Space, the arrows, W and E reach nothing.
namespace focus
{
inline void takeBack (juce::Component* c)
{
    if (c == nullptr)
        return;
    auto* editor = c->findParentComponentOfClass<juce::AudioProcessorEditor>();
    if (editor == nullptr)
        editor = dynamic_cast<juce::AudioProcessorEditor*> (c);
    if (editor == nullptr || ! editor->isShowing())
        return;
    if (auto* peer = editor->getPeer(); peer != nullptr && ! peer->isFocused())
        peer->grabFocus();
    if (! editor->hasKeyboardFocus (true))
        editor->grabKeyboardFocus();
}

// For PopupMenu::showMenuAsync: once the menu has closed.
inline std::function<void (int)> afterMenu (juce::Component* c)
{
    juce::Component::SafePointer<juce::Component> safe (c);
    return [safe] (int)
    {
        juce::MessageManager::callAsync ([safe] { takeBack (safe.getComponent()); });
    };
}
} // namespace focus
