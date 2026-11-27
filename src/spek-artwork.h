#pragma once

#define ART_SPEK "art-spek"
#define ART_OPEN "art-open"
#define ART_SAVE "art-save"
#define ART_HELP "art-help"
#define ART_CLOSE "art-close"

#include <wx/artprov.h>
#include <wx/bmpbndl.h>

void spek_artwork_init();

// Toolbar and button icons. On macOS they're monochrome, tinted to suit the
// current light or dark appearance, so fetch them again when it changes.
wxBitmapBundle spek_artwork_icon(const wxArtID& id, const wxArtClient& client);
