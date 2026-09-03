#include "Features.h"

/* Constant-initialised on purpose, so the chain is a valid empty list before
   any Feature constructor can run. See the note in Features.h - this must not
   become a runtime initialiser. */
Feature* Feature::_head = nullptr;

Feature::Feature() : _next(_head) {
  _head = this;
}

void Feature::SetupAll() {
  for (Feature* f = _head; f != nullptr; f = f->_next)
    f->Setup();
}

void Feature::LoopAll() {
  for (Feature* f = _head; f != nullptr; f = f->_next)
    f->Loop();
}

void Feature::BuildDocAll(JsonDocument& doc, bool all) {
  for (Feature* f = _head; f != nullptr; f = f->_next)
    f->BuildDoc(doc, all);
}

bool Feature::HandleWSAll(JsonDocument& doc) {
  /* Every feature is offered the message even after one has claimed it: keys
     are namespaced per feature, so there is nothing to stop two features
     answering different keys of the same batched update - the settings page
     sends several at once. Short-circuiting on the first true would silently
     drop the rest. */
  bool handled = false;
  for (Feature* f = _head; f != nullptr; f = f->_next)
    if (f->HandleWS(doc))
      handled = true;
  return handled;
}

uint8_t Feature::Count() {
  uint8_t n = 0;
  for (Feature* f = _head; f != nullptr; f = f->_next)
    n++;
  return n;
}
