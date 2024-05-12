#include "simulator.h"

#include "ta/ta.h"
#include <fontconfig/fontconfig.h>

struct fontconfig_priv {
    FcConfig *config;
};

static void do_destroy_priv(void *p)
{
    struct fontconfig_priv *priv = p;
    if (priv->config)
        FcConfigDestroy(priv->config);
}

void simulator_fontconfig_init(void *parent, void **fc)
{
    struct fontconfig_priv *priv = ta_new_ptrtype(parent, priv);
    ta_set_destructor(priv, do_destroy_priv);
    priv->config = FcInitLoadConfigAndFonts();
    *fc = priv;
}

bool simulator_fontconfig_select(void *fc, int codepoint, char **best_path, int *best_idx)
{
    FcPattern *pattern = NULL;
    FcCharSet *charset = NULL;
    FcPattern *matched = NULL;

    if (!(fc && best_path && best_idx))
        goto done;

    struct fontconfig_priv *priv = fc;
    pattern = FcPatternCreate();
    charset = FcCharSetCreate();
    if (!(pattern && charset))
        goto done;

    FcCharSetAddChar(charset, codepoint);
    FcPatternAddCharSet(pattern, FC_CHARSET, charset);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcPatternAddInteger(pattern, FC_SPACING, FC_MONO);

    FcConfigSubstitute(priv->config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    matched = FcFontMatch(priv->config, pattern, &result);
    if (result != FcResultMatch)
        goto done;

    FcChar8 *file = NULL;
    if (FcPatternGetString(matched, FC_FILE, 0, &file) != FcResultMatch)
        goto done;

    int index = 0;
    if (FcPatternGetInteger(matched, FC_INDEX, 0, &index) != FcResultMatch)
        goto done;

    *best_path = ta_strdup(priv, (const char*) file);
    *best_idx = index;

done:
    if (pattern)
        FcPatternDestroy(pattern);
    if (charset)
        FcCharSetDestroy(charset);
    if (matched)
        FcPatternDestroy(matched);
    return true;
}
