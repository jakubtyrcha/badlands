#include <shapeshifter/ShapeshifterCore.h>

namespace sq {

Editor* Editor::create() { return new Editor(); }

int32_t Editor::ping() const { return 42; }

} // namespace sq
