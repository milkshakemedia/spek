#include <wx/artprov.h>
#include <wx/iconbndl.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/stdpaths.h>

#include "spek-artwork.h"

class SpekArtProvider : public wxArtProvider
{
protected:
    virtual wxBitmap CreateBitmap(const wxArtID& id, const wxArtClient& client, const wxSize& size);
    virtual wxIconBundle CreateIconBundle(const wxArtID& id, const wxArtClient& client);
};

wxBitmap SpekArtProvider::CreateBitmap(
    const wxArtID& id, const wxArtClient& client, const wxSize& size)
{
	(void) id, (void) client, (void) size;
#ifdef OS_UNIX
    if (id == ART_SPEK) {
        return wxArtProvider::GetBitmap("spek", client, size);
    }
    if (id == ART_HELP) {
        return wxArtProvider::GetBitmap("gtk-help", client, size);
    }
    if (id == ART_OPEN) {
        return wxArtProvider::GetBitmap("gtk-open", client, size);
    }
    if (id == ART_SAVE) {
        return wxArtProvider::GetBitmap("gtk-save", client, size);
    }
    if (id == ART_CLOSE) {
        return wxArtProvider::GetBitmap("gtk-close", client, size);
    }
#endif
#ifdef OS_WIN
    if (id == ART_HELP) {
        return wxIcon("help", wxBITMAP_TYPE_ICO_RESOURCE, 24, 24);
    }
    if (id == ART_OPEN) {
        return wxIcon("open", wxBITMAP_TYPE_ICO_RESOURCE, 24, 24);
    }
    if (id == ART_SAVE) {
        return wxIcon("save", wxBITMAP_TYPE_ICO_RESOURCE, 24, 24);
    }
    if (id == ART_CLOSE) {
        return wxIcon("close", wxBITMAP_TYPE_ICO_RESOURCE, 16, 16);
    }
#endif
    return wxNullBitmap;
}

wxIconBundle SpekArtProvider::CreateIconBundle(const wxArtID& id, const wxArtClient& client)
{
	(void) id, (void) client;
#ifdef OS_UNIX
    if (id == ART_SPEK) {
        return wxArtProvider::GetIconBundle("spek", client);
    }
#endif
#ifdef OS_WIN
    if (id == ART_SPEK) {
        wxIconBundle bundle;
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 16, 16));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 24, 24));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 32, 32));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 48, 48));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 64, 64));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 96, 96));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 128, 128));
        bundle.AddIcon(wxIcon("aaaa", wxBITMAP_TYPE_ICO_RESOURCE, 256, 256));
        return bundle;
    }
#endif
    return wxNullIconBundle;
}

#ifdef OS_OSX
// Line icons drawn in black on transparent (dist/icons), at 1x and 2x,
// recoloured like the system's own toolbar symbols.
static wxBitmapBundle tinted(const wxString& name)
{
    const wxString dir = wxStandardPaths::Get().GetResourcesDir();
    const bool dark = wxSystemSettings::GetAppearance().IsDark();
    const unsigned char level = dark ? 255 : 0;
    const double opacity = dark ? 0.85 : 0.78;
    wxVector<wxBitmap> bitmaps;
    for (const char *suffix : { "", "@2x" }) {
        wxImage img;
        if (!img.LoadFile(dir + "/" + name + suffix + ".png", wxBITMAP_TYPE_PNG)) continue;
        if (!img.HasAlpha()) img.InitAlpha();
        unsigned char *rgb = img.GetData(), *alpha = img.GetAlpha();
        const int n = img.GetWidth() * img.GetHeight();
        for (int i = 0; i < n; ++i) {
            rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = level;
            alpha[i] = (unsigned char)(alpha[i] * opacity + 0.5);
        }
        bitmaps.push_back(wxBitmap(img));
    }
    return wxBitmapBundle::FromBitmaps(bitmaps);
}
#endif

wxBitmapBundle spek_artwork_icon(const wxArtID& id, const wxArtClient& client)
{
#ifdef OS_OSX
    if (id == ART_OPEN) return tinted("open");
    if (id == ART_SAVE) return tinted("save");
    if (id == ART_HELP) return tinted("help");
    if (id == ART_CLOSE) return tinted("close");
#endif
    return wxArtProvider::GetBitmapBundle(id, client);
}

void spek_artwork_init()
{
    wxArtProvider::Push(new SpekArtProvider());
}
