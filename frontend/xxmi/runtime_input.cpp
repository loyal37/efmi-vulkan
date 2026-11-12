// Preserve original key parsing, modifiers, cycle/hold/toggle and edge dispatch.
// Replace only the OS polling source with the host's one coherent input sample.
#include "Input.h"
#include "runtime.h"
// The dispatching thread's sample; each thread dispatches the generation it entered (D-129).
static __declspec(thread) const efmi::frontend::RuntimeInput* runtime_input = nullptr;
static SHORT WINAPI RuntimeGetAsyncKeyState(int key)
{
    return runtime_input && key >= 0 && key < 256 && runtime_input->keys[key] ? SHORT(0x8000) : 0;
}
// The original's key actions are the entered generation's (engine_globals.h for the shared names;
// this one is local to input.cpp, whose `static` declaration becomes the accessor's).
std::vector<InputAction*>* RuntimeInputActions();
#define GetAsyncKeyState RuntimeGetAsyncKeyState
#define actions (*efvk_engine_actions())
#include "input.cpp"
#undef GetAsyncKeyState
static std::vector<InputAction*>* efvk_engine_actions() { return RuntimeInputActions(); }

size_t RuntimeInputCount() { return actions.size(); }

// The keys each action needs held, read off the original's parsed buttons, for a host's check that
// presses them as a player does (D-137): the original's own dispatch then decides what they change.
// InputButtonList keeps its buttons private; the standard's explicit instantiation reads them without
// changing the original.
namespace {
template <typename Tag, typename Tag::type Member> struct Expose { friend typename Tag::type member(Tag) { return Member; } };
struct ButtonsOf { using type = std::vector<InputButton*> InputButtonList::*; friend type member(ButtonsOf); };
template struct Expose<ButtonsOf, &InputButtonList::buttons>;
void held_keys(InputButton* button, std::vector<int>& out, bool& usable)
{
    if (auto* key = dynamic_cast<VKInputButton*>(button)) { if (!key->invert) out.push_back(key->vkey); return; }
    if (auto* list = dynamic_cast<InputButtonList*>(button)) { for (auto* b : list->*member(ButtonsOf())) held_keys(b, out, usable); return; }
    usable = false; // a controller button: no key to hold
}
}
// The key actions from `skip_first` to `skip_last` are left out (the host's hunting keys, docs/vulkan-native-mods.md).
std::vector<std::vector<int>> RuntimeInputKeys(size_t skip_first, size_t skip_last)
{
    std::vector<std::vector<int>> out;
    for (size_t n = 0; n < actions.size(); ++n) {
        if (n >= skip_first && n < skip_last) continue;
        auto* action = actions[n];
        std::vector<int> keys; bool usable = true;
        if (action && action->button) held_keys(action->button, keys, usable);
        if (usable && !keys.empty()) out.push_back(std::move(keys));
    }
    return out;
}
bool RuntimeDispatchInput(HackerDevice* device, const efmi::frontend::RuntimeInput& input)
{
    efmi::frontend::RuntimeInput released;
    runtime_input = input.foreground ? &input : &released;
    bool changed = false;
    try { for (auto* action : actions) changed |= action->Dispatch(device); }
    catch (...) { runtime_input = nullptr; throw; }
    runtime_input = nullptr;
    return changed;
}
