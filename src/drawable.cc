#include "drawable.hh"

#include "drawable_nortti.hh"

namespace automat {

Drawable::Drawable() : sk(new SkDrawableWrapper(this)) {}
void Drawable::draw(SkCanvas* c, SkScalar x, SkScalar y) { sk->draw(c, x, y); }

}  // namespace automat