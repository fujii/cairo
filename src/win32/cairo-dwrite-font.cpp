/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* Cairo - a vector graphics library with display and print output
 *
 * Copyright © 2010 Mozilla Foundation
 * Copyright © 2020 Sony Interactive Entertainment Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is the Mozilla Foundation
 *
 * Contributor(s):
 *	Bas Schouten <bschouten@mozilla.com>
 */

#include "cairoint.h"

#include "cairo-win32-private.h"
#include "cairo-image-surface-inline.h"
#include "cairo-image-surface-private.h"
#include "cairo-clip-private.h"
#include "cairo-win32-refptr.h"
#include "cairo-pattern-private.h"

#include "cairo-dwrite-private.h"
#include "cairo-truetype-subset-private.h"
#include <float.h>

typedef HRESULT (WINAPI*D2D1CreateFactoryFunc)(
    D2D1_FACTORY_TYPE factoryType,
    REFIID iid,
    CONST D2D1_FACTORY_OPTIONS *pFactoryOptions,
    void **factory
);

// Forward declarations
static cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_d2d(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       cairo_dwrite_scaled_font_t *scaled_font,
				       const RECT &area);

static cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_gdi(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       cairo_dwrite_scaled_font_t *scaled_font,
				       const RECT &area);

class D2DFactory
{
public:
    static ID2D1Factory *Instance()
    {
	if (!mFactoryInstance) {
	    D2D1CreateFactoryFunc createD2DFactory = (D2D1CreateFactoryFunc)
		GetProcAddress(LoadLibraryW(L"d2d1.dll"), "D2D1CreateFactory");
	    if (createD2DFactory) {
		D2D1_FACTORY_OPTIONS options;
		options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
		createD2DFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				 __uuidof(ID2D1Factory),
				 &options,
				 (void**)&mFactoryInstance);
	    }
	}
	return mFactoryInstance;
    }

    static ID2D1DCRenderTarget *RenderTarget()
    {
	if (!mRenderTarget) {
	    if (!Instance()) {
		return NULL;
	    }
	    // Create a DC render target.
	    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(
		    DXGI_FORMAT_B8G8R8A8_UNORM,
		    D2D1_ALPHA_MODE_PREMULTIPLIED),
		0,
		0,
		D2D1_RENDER_TARGET_USAGE_NONE,
		D2D1_FEATURE_LEVEL_DEFAULT
		);

	    Instance()->CreateDCRenderTarget(&props, &mRenderTarget);
	}
	return mRenderTarget;
    }

private:
    static ID2D1Factory *mFactoryInstance;
    static ID2D1DCRenderTarget *mRenderTarget;
};

IDWriteFactory *DWriteFactory::mFactoryInstance = NULL;
IDWriteFontCollection *DWriteFactory::mSystemCollection = NULL;

ID2D1Factory *D2DFactory::mFactoryInstance = NULL;
ID2D1DCRenderTarget *D2DFactory::mRenderTarget = NULL;

/* Functions cairo_font_face_backend_t */
static cairo_status_t
_cairo_dwrite_font_face_create_for_toy (cairo_toy_font_face_t   *toy_face,
					cairo_font_face_t      **font_face);
static cairo_bool_t
_cairo_dwrite_font_face_destroy (void *font_face);

static cairo_status_t
_cairo_dwrite_font_face_scaled_font_create (void			*abstract_face,
					    const cairo_matrix_t	*font_matrix,
					    const cairo_matrix_t	*ctm,
					    const cairo_font_options_t *options,
					    cairo_scaled_font_t **font);

const cairo_font_face_backend_t _cairo_dwrite_font_face_backend = {
    CAIRO_FONT_TYPE_DWRITE,
    _cairo_dwrite_font_face_create_for_toy,
    _cairo_dwrite_font_face_destroy,
    _cairo_dwrite_font_face_scaled_font_create
};

/* Functions cairo_scaled_font_backend_t */

static void _cairo_dwrite_scaled_font_fini(void *scaled_font);

static cairo_warn cairo_int_status_t
_cairo_dwrite_scaled_glyph_init(void			     *scaled_font,
				cairo_scaled_glyph_t	     *scaled_glyph,
				cairo_scaled_glyph_info_t    info);

static cairo_int_status_t
_cairo_dwrite_load_truetype_table(void		       *scaled_font,
				  unsigned long         tag,
				  long                  offset,
				  unsigned char        *buffer,
				  unsigned long        *length);

static unsigned long
_cairo_dwrite_ucs4_to_index(void			     *scaled_font,
			    uint32_t			ucs4);


static cairo_bool_t
_cairo_dwrite_has_color_glyphs(void	*scaled_font);

const cairo_scaled_font_backend_t _cairo_dwrite_scaled_font_backend = {
    CAIRO_FONT_TYPE_DWRITE,
    _cairo_dwrite_scaled_font_fini,
    _cairo_dwrite_scaled_glyph_init,
    NULL,
    _cairo_dwrite_ucs4_to_index,
    _cairo_dwrite_load_truetype_table,
    NULL,
    NULL,
    NULL,
    NULL,
    _cairo_dwrite_has_color_glyphs,
};

/* Helper conversion functions */

/**
 * Get a D2D matrix from a cairo matrix. Note that D2D uses row vectors where cairo
 * uses column vectors. Hence the transposition.
 *
 * \param Cairo matrix
 * \return D2D matrix
 */
static D2D1::Matrix3x2F
_cairo_d2d_matrix_from_matrix(const cairo_matrix_t *matrix)
{
    return D2D1::Matrix3x2F((FLOAT)matrix->xx,
			    (FLOAT)matrix->yx,
			    (FLOAT)matrix->xy,
			    (FLOAT)matrix->yy,
			    (FLOAT)matrix->x0,
			    (FLOAT)matrix->y0);
}


/**
 * Get a DirectWrite matrix from a cairo matrix. Note that DirectWrite uses row
 * vectors where cairo uses column vectors. Hence the transposition.
 *
 * \param Cairo matrix
 * \return DirectWrite matrix
 */
static DWRITE_MATRIX
_cairo_dwrite_matrix_from_matrix(const cairo_matrix_t *matrix)
{
    DWRITE_MATRIX dwmat;
    dwmat.m11 = (FLOAT)matrix->xx;
    dwmat.m12 = (FLOAT)matrix->yx;
    dwmat.m21 = (FLOAT)matrix->xy;
    dwmat.m22 = (FLOAT)matrix->yy;
    dwmat.dx = (FLOAT)matrix->x0;
    dwmat.dy = (FLOAT)matrix->y0;
    return dwmat;
}

/* Helper functions for cairo_dwrite_scaled_glyph_init */
static cairo_int_status_t 
_cairo_dwrite_scaled_font_init_glyph_metrics 
    (cairo_dwrite_scaled_font_t *scaled_font, cairo_scaled_glyph_t *scaled_glyph);

static cairo_int_status_t 
_cairo_dwrite_scaled_font_init_glyph_surface
    (cairo_dwrite_scaled_font_t *scaled_font, cairo_scaled_glyph_t *scaled_glyph);

static cairo_int_status_t 
_cairo_dwrite_scaled_font_init_glyph_path
    (cairo_dwrite_scaled_font_t *scaled_font, cairo_scaled_glyph_t *scaled_glyph);

/* implement the font backend interface */

static cairo_status_t
_cairo_dwrite_font_face_create_for_toy (cairo_toy_font_face_t   *toy_face,
					cairo_font_face_t      **font_face)
{
    WCHAR *face_name;
    int face_name_len;

    if (!DWriteFactory::Instance()) {
	return (cairo_status_t)CAIRO_INT_STATUS_UNSUPPORTED;
    }

    face_name_len = MultiByteToWideChar(CP_UTF8, 0, toy_face->family, -1, NULL, 0);
    face_name = new WCHAR[face_name_len];
    MultiByteToWideChar(CP_UTF8, 0, toy_face->family, -1, face_name, face_name_len);

    IDWriteFontFamily *family = DWriteFactory::FindSystemFontFamily(face_name);
    delete face_name;
    if (!family) {
	*font_face = (cairo_font_face_t*)&_cairo_font_face_nil;
	return CAIRO_STATUS_FONT_TYPE_MISMATCH;
    }

    DWRITE_FONT_WEIGHT weight;
    switch (toy_face->weight) {
    case CAIRO_FONT_WEIGHT_BOLD:
	weight = DWRITE_FONT_WEIGHT_BOLD;
	break;
    case CAIRO_FONT_WEIGHT_NORMAL:
    default:
	weight = DWRITE_FONT_WEIGHT_NORMAL;
	break;
    }

    DWRITE_FONT_STYLE style;
    switch (toy_face->slant) {
    case CAIRO_FONT_SLANT_ITALIC:
	style = DWRITE_FONT_STYLE_ITALIC;
	break;
    case CAIRO_FONT_SLANT_OBLIQUE:
	style = DWRITE_FONT_STYLE_OBLIQUE;
	break;
    case CAIRO_FONT_SLANT_NORMAL:
    default:
	style = DWRITE_FONT_STYLE_NORMAL;
	break;
    }

    IDWriteFont* dwfont;
    IDWriteFontFace* dwface;
    HRESULT hr = family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &dwfont);
    if (FAILED(hr))
	return CAIRO_STATUS_FONT_TYPE_MISMATCH;

    hr = dwfont->CreateFontFace(&dwface);
    if (FAILED(hr))
	return CAIRO_STATUS_FONT_TYPE_MISMATCH;

    // Cannot use C++ style new since cairo deallocates this.
    cairo_dwrite_font_face_t* face = (cairo_dwrite_font_face_t*) malloc(sizeof(cairo_dwrite_font_face_t));
    *font_face = (cairo_font_face_t*)face;
    face->font = dwfont;
    face->dwriteface = dwface;
    face->rendering_mode = DWRITE_RENDERING_MODE_DEFAULT;
    face->rendering_params = NULL;
    _cairo_font_face_init (&(*(_cairo_dwrite_font_face**)font_face)->base, &_cairo_dwrite_font_face_backend);
    return CAIRO_STATUS_SUCCESS;
}

static cairo_bool_t
_cairo_dwrite_font_face_destroy (void *font_face)
{
    cairo_dwrite_font_face_t *dwrite_font_face = static_cast<cairo_dwrite_font_face_t*>(font_face);
    if (dwrite_font_face->dwriteface)
	dwrite_font_face->dwriteface->Release();
    if (dwrite_font_face->font)
	dwrite_font_face->font->Release();
    if (dwrite_font_face->rendering_params)
	dwrite_font_face->rendering_params->Release();
    return TRUE;
}


static inline unsigned short
read_short(const char *buf)
{
    return be16_to_cpu(*(unsigned short*)buf);
}

static cairo_bool_t _dwrite_scaled_font_uses_gdi (cairo_dwrite_scaled_font_t *scaled_font, BOOL* gdi_natural)
{
    cairo_dwrite_font_face_t *font_face = (cairo_dwrite_font_face_t*)scaled_font->base.font_face;
    cairo_bool_t use_gdi = FALSE;
    cairo_bool_t use_gdi_natural = FALSE;
    switch (font_face->rendering_mode) {
    case DWRITE_RENDERING_MODE_GDI_NATURAL:
	use_gdi_natural = TRUE;
	/* FALLTHROUGH */
    case DWRITE_RENDERING_MODE_GDI_CLASSIC:
	use_gdi = TRUE;
	break;
    default:
	break;
    }
    if (gdi_natural)
	*gdi_natural = use_gdi_natural;
    return use_gdi;
}

static cairo_status_t
_cairo_dwrite_font_face_scaled_font_create (void			*abstract_face,
					    const cairo_matrix_t	*font_matrix,
					    const cairo_matrix_t	*ctm,
					    const cairo_font_options_t  *options,
					    cairo_scaled_font_t **font)
{
    cairo_dwrite_font_face_t *font_face = static_cast<cairo_dwrite_font_face_t*>(abstract_face);

    // Must do malloc and not C++ new, since Cairo frees this.
    cairo_dwrite_scaled_font_t *dwriteFont = (cairo_dwrite_scaled_font_t*)malloc(sizeof(cairo_dwrite_scaled_font_t));
    *font = reinterpret_cast<cairo_scaled_font_t*>(dwriteFont);
    _cairo_scaled_font_init(&dwriteFont->base, &font_face->base, font_matrix, ctm, options, &_cairo_dwrite_scaled_font_backend);

    dwriteFont->mat = dwriteFont->base.ctm;
    cairo_matrix_multiply(&dwriteFont->mat, &dwriteFont->mat, font_matrix);
    dwriteFont->mat_inverse = dwriteFont->mat;
    cairo_matrix_invert (&dwriteFont->mat_inverse);

    cairo_font_extents_t extents;

    DWRITE_FONT_METRICS metrics;
    if (_dwrite_scaled_font_uses_gdi (dwriteFont, NULL)) {
	DWRITE_MATRIX transform = _cairo_dwrite_matrix_from_matrix (&dwriteFont->mat);
	font_face->dwriteface->GetGdiCompatibleMetrics (1, 1, &transform, &metrics);
    } else
	font_face->dwriteface->GetMetrics(&metrics);

    extents.ascent = (FLOAT)metrics.ascent / metrics.designUnitsPerEm;
    extents.descent = (FLOAT)metrics.descent / metrics.designUnitsPerEm;
    extents.height = (FLOAT)(metrics.ascent + metrics.descent + metrics.lineGap) / metrics.designUnitsPerEm;
    extents.max_x_advance = 14.0;
    extents.max_y_advance = 0.0;

    return _cairo_scaled_font_set_metrics (*font, &extents);
}

static IDWriteRenderingParams *
_cairo_dwrite_font_face_get_rendering_params (cairo_dwrite_font_face_t *dwrite_font_face)
{
    if (!dwrite_font_face->rendering_params) {
	RefPtr<IDWriteRenderingParams> params;
	DWriteFactory::Instance()->CreateRenderingParams(&params);
	if (dwrite_font_face->rendering_mode != DWRITE_RENDERING_MODE_DEFAULT) {
	    FLOAT gamma = params->GetGamma();
	    FLOAT contrast = params->GetEnhancedContrast();
	    FLOAT level = params->GetClearTypeLevel();
	    DWRITE_PIXEL_GEOMETRY geometry = params->GetPixelGeometry();
	    DWRITE_RENDERING_MODE mode = dwrite_font_face->rendering_mode;
	    DWriteFactory::Instance()->CreateCustomRenderingParams(gamma, contrast, level,
								   geometry, mode,
								   &params);
	}
	dwrite_font_face->rendering_params = params.forget().drop();
    }
    return dwrite_font_face->rendering_params;
}


/* Implementation cairo_dwrite_scaled_font_backend_t */
static void
_cairo_dwrite_scaled_font_fini(void *scaled_font)
{
}

static cairo_int_status_t
_cairo_dwrite_scaled_glyph_init(void			     *scaled_font,
				cairo_scaled_glyph_t	     *scaled_glyph,
				cairo_scaled_glyph_info_t    info)
{
    cairo_dwrite_scaled_font_t *scaled_dwrite_font = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_int_status_t status;

    if ((info & CAIRO_SCALED_GLYPH_INFO_METRICS) != 0) {
	status = _cairo_dwrite_scaled_font_init_glyph_metrics (scaled_dwrite_font, scaled_glyph);
	if (status)
	    return status;
    }

    if (info & CAIRO_SCALED_GLYPH_INFO_SURFACE) {
	status = _cairo_dwrite_scaled_font_init_glyph_surface (scaled_dwrite_font, scaled_glyph);
	if (status)
	    return status;
    }

    if ((info & CAIRO_SCALED_GLYPH_INFO_PATH) != 0) {
	status = _cairo_dwrite_scaled_font_init_glyph_path (scaled_dwrite_font, scaled_glyph);
	if (status)
	    return status;
    }

    return CAIRO_INT_STATUS_SUCCESS;
}

static unsigned long
_cairo_dwrite_ucs4_to_index(void			     *scaled_font,
			    uint32_t		      ucs4)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *face = reinterpret_cast<cairo_dwrite_font_face_t*>(dwritesf->base.font_face);

    UINT16 index;
    face->dwriteface->GetGlyphIndicesA(&ucs4, 1, &index);
    return index;
}

/* cairo_dwrite_scaled_glyph_init helper function bodies */
static cairo_int_status_t 
_cairo_dwrite_scaled_font_init_glyph_metrics(cairo_dwrite_scaled_font_t *scaled_font, 
					     cairo_scaled_glyph_t *scaled_glyph)
{
    UINT16 charIndex = (UINT16)_cairo_scaled_glyph_index (scaled_glyph);
    cairo_dwrite_font_face_t *font_face = (cairo_dwrite_font_face_t*)scaled_font->base.font_face;
    cairo_text_extents_t extents;

    DWRITE_GLYPH_METRICS metrics;
    DWRITE_FONT_METRICS fontMetrics;
    HRESULT hr;
    BOOL use_gdi_natural;
    if (_dwrite_scaled_font_uses_gdi (scaled_font, &use_gdi_natural)) {
	DWRITE_MATRIX transform = _cairo_dwrite_matrix_from_matrix (&scaled_font->mat);
	font_face->dwriteface->GetGdiCompatibleMetrics (1, 1, &transform, &fontMetrics);
	hr = font_face->dwriteface->GetGdiCompatibleGlyphMetrics (1, 1, &transform, use_gdi_natural, &charIndex, 1, &metrics, FALSE);
    } else {
	font_face->dwriteface->GetMetrics(&fontMetrics);
	hr = font_face->dwriteface->GetDesignGlyphMetrics(&charIndex, 1, &metrics);
    }
    if (FAILED(hr)) {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    // TODO: Treat swap_xy.
    extents.width = (FLOAT)(metrics.advanceWidth - metrics.leftSideBearing - metrics.rightSideBearing) /
	fontMetrics.designUnitsPerEm;
    extents.height = (FLOAT)(metrics.advanceHeight - metrics.topSideBearing - metrics.bottomSideBearing) /
	fontMetrics.designUnitsPerEm;
    extents.x_advance = (FLOAT)metrics.advanceWidth / fontMetrics.designUnitsPerEm;
    extents.x_bearing = (FLOAT)metrics.leftSideBearing / fontMetrics.designUnitsPerEm;
    extents.y_advance = 0.0;
    extents.y_bearing = (FLOAT)(metrics.topSideBearing - metrics.verticalOriginY) /
	fontMetrics.designUnitsPerEm;

    // We pad the extents here because GetDesignGlyphMetrics returns "ideal" metrics
    // for the glyph outline, without accounting for hinting/gridfitting/antialiasing,
    // and therefore it does not always cover all pixels that will actually be touched.
    if (scaled_font->base.options.antialias != CAIRO_ANTIALIAS_NONE &&
	extents.width > 0 && extents.height > 0) {
	extents.width += scaled_font->mat_inverse.xx * 2;
	extents.x_bearing -= scaled_font->mat_inverse.xx;
    }

    _cairo_scaled_glyph_set_metrics (scaled_glyph,
				     &scaled_font->base,
				     &extents);
    return CAIRO_INT_STATUS_SUCCESS;
}

/**
 * Stack-based helper implementing IDWriteGeometrySink.
 * Used to determine the path of the glyphs.
 */

class GeometryRecorder : public IDWriteGeometrySink
{
public:
    GeometryRecorder(cairo_path_fixed_t *aCairoPath) 
	: mCairoPath(aCairoPath) {}

    // IUnknown interface
    IFACEMETHOD(QueryInterface)(IID const& iid, OUT void** ppObject)
    {
	if (iid != __uuidof(IDWriteGeometrySink))
	    return E_NOINTERFACE;

	*ppObject = static_cast<IDWriteGeometrySink*>(this);

	return S_OK;
    }

    IFACEMETHOD_(ULONG, AddRef)()
    {
	return 1;
    }

    IFACEMETHOD_(ULONG, Release)()
    {
	return 1;
    }

    IFACEMETHODIMP_(void) SetFillMode(D2D1_FILL_MODE fillMode)
    {
	return;
    }

    STDMETHODIMP Close()
    {
	return S_OK;
    }

    IFACEMETHODIMP_(void) SetSegmentFlags(D2D1_PATH_SEGMENT vertexFlags)
    {
	return;
    }
    
    cairo_fixed_t GetFixedX(const D2D1_POINT_2F &point)
    {
#ifdef _M_IX86
	unsigned int control_word;
	_controlfp_s(&control_word, _CW_DEFAULT, MCW_PC);
#endif
	return _cairo_fixed_from_double(point.x);
    }

    cairo_fixed_t GetFixedY(const D2D1_POINT_2F &point)
    {
#ifdef _M_IX86
	unsigned int control_word;
	_controlfp_s(&control_word, _CW_DEFAULT, MCW_PC);
#endif
	return _cairo_fixed_from_double(point.y);
    }

    IFACEMETHODIMP_(void) BeginFigure(
	D2D1_POINT_2F startPoint, 
	D2D1_FIGURE_BEGIN figureBegin) 
    {
	mStartPoint = startPoint;
	cairo_status_t status = _cairo_path_fixed_move_to(mCairoPath, 
							  GetFixedX(startPoint),
							  GetFixedY(startPoint));
    }

    IFACEMETHODIMP_(void) EndFigure(    
	D2D1_FIGURE_END figureEnd) 
    {
	if (figureEnd == D2D1_FIGURE_END_CLOSED) {
	    cairo_status_t status = _cairo_path_fixed_line_to(mCairoPath,
							      GetFixedX(mStartPoint), 
							      GetFixedY(mStartPoint));
	}
    }

    IFACEMETHODIMP_(void) AddBeziers(
	const D2D1_BEZIER_SEGMENT *beziers,
	UINT beziersCount)
    {
	for (unsigned int i = 0; i < beziersCount; i++) {
	    cairo_status_t status = _cairo_path_fixed_curve_to(mCairoPath,
							       GetFixedX(beziers[i].point1),
							       GetFixedY(beziers[i].point1),
							       GetFixedX(beziers[i].point2),
							       GetFixedY(beziers[i].point2),
							       GetFixedX(beziers[i].point3),
							       GetFixedY(beziers[i].point3));
	}	
    }

    IFACEMETHODIMP_(void) AddLines(
	const D2D1_POINT_2F *points,
	UINT pointsCount)
    {
	for (unsigned int i = 0; i < pointsCount; i++) {
	    cairo_status_t status = _cairo_path_fixed_line_to(mCairoPath, 
		GetFixedX(points[i]), 
		GetFixedY(points[i]));
	}
    }

private:
    cairo_path_fixed_t *mCairoPath;
    D2D1_POINT_2F mStartPoint;
};

static cairo_int_status_t 
_cairo_dwrite_scaled_font_init_glyph_path(cairo_dwrite_scaled_font_t *scaled_font, 
					  cairo_scaled_glyph_t *scaled_glyph)
{
    cairo_path_fixed_t *path;
    path = _cairo_path_fixed_create();
    GeometryRecorder recorder(path);

    DWRITE_GLYPH_OFFSET offset;
    offset.advanceOffset = 0;
    offset.ascenderOffset = 0;
    UINT16 glyphId = (UINT16)_cairo_scaled_glyph_index(scaled_glyph);
    FLOAT advance = 0.0;
    cairo_dwrite_font_face_t *dwriteff = (cairo_dwrite_font_face_t*)scaled_font->base.font_face;
    dwriteff->dwriteface->GetGlyphRunOutline((FLOAT)scaled_font->base.font_matrix.yy,
					     &glyphId,
					     &advance,
					     &offset,
					     1,
					     FALSE,
					     FALSE,
					     &recorder);
    _cairo_path_fixed_close_path(path);

    /* Now apply our transformation to the drawn path. */
    _cairo_path_fixed_transform(path, &scaled_font->base.ctm);
    
    _cairo_scaled_glyph_set_path (scaled_glyph,
				  &scaled_font->base,
				  path);
    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_clone_image_surface(cairo_format_t format,
		     cairo_surface_t *surface,
		     cairo_rectangle_int_t *extents,
		     cairo_image_surface_t **out_image)
{
    cairo_surface_t *image;
    cairo_surface_pattern_t pattern;
    cairo_status_t status;

    image = cairo_image_surface_create (format,
					extents->width,
					extents->height);
    if (image->status)
	return CAIRO_INT_STATUS_NO_MEMORY;

    /* TODO: check me with non-identity device_transform. Should we
     * clone the scaling, too? */
    cairo_surface_set_device_offset (image,
				     -extents->x,
				     -extents->y);

    _cairo_pattern_init_for_surface (&pattern, surface);
    pattern.base.filter = CAIRO_FILTER_NEAREST;

    status = _cairo_surface_paint (image,
				   CAIRO_OPERATOR_SOURCE,
				   &pattern.base,
				   NULL);

    _cairo_pattern_fini (&pattern.base);

    *out_image = (cairo_image_surface_t *)image;
    return CAIRO_INT_STATUS_SUCCESS;
}

static DWRITE_MEASURING_MODE _dwrite_scaled_font_mesuaring_mode (cairo_dwrite_scaled_font_t *scaled_font)
{
    cairo_dwrite_font_face_t *font_face = (cairo_dwrite_font_face_t*)scaled_font->base.font_face;
    switch (font_face->rendering_mode) {
    case DWRITE_RENDERING_MODE_GDI_CLASSIC:
	return DWRITE_MEASURING_MODE_GDI_CLASSIC;
    case DWRITE_RENDERING_MODE_GDI_NATURAL:
	return DWRITE_MEASURING_MODE_GDI_NATURAL;
    default:
	return DWRITE_MEASURING_MODE_NATURAL;
    }
}

static cairo_int_status_t 
_cairo_dwrite_scaled_font_init_glyph_surface(cairo_dwrite_scaled_font_t *scaled_font, 
					     cairo_scaled_glyph_t	*scaled_glyph)
{
    cairo_int_status_t status;
    cairo_glyph_t glyph;
    cairo_surface_t *surface = NULL;
    cairo_t *cr;
    cairo_image_surface_t *image;
    int width, height;
    double x1, y1, x2, y2;

    x1 = _cairo_fixed_integer_floor (scaled_glyph->bbox.p1.x);
    y1 = _cairo_fixed_integer_floor (scaled_glyph->bbox.p1.y);
    x2 = _cairo_fixed_integer_ceil (scaled_glyph->bbox.p2.x);
    y2 = _cairo_fixed_integer_ceil (scaled_glyph->bbox.p2.y);
    width = (int)(x2 - x1);
    height = (int)(y2 - y1);

    glyph.index = _cairo_scaled_glyph_index (scaled_glyph);
    glyph.x = -x1;
    glyph.y = -y1;

    DWRITE_GLYPH_RUN run;
    FLOAT advance = 0;
    UINT16 index = (UINT16)glyph.index;
    DWRITE_GLYPH_OFFSET offset;
    double x = glyph.x;
    double y = glyph.y;
    RECT area;
    DWRITE_MATRIX matrix;
    cairo_rectangle_int_t extents = { 0, 0, width, height };

    surface = cairo_win32_surface_create_with_dib (CAIRO_FORMAT_ARGB32,
						   width, height);

    status = (cairo_int_status_t)_cairo_surface_paint (surface, CAIRO_OPERATOR_SOURCE,
						       &_cairo_pattern_clear.base, NULL);
    if (status)
	goto FAIL;

    /**
     * We transform by the inverse transformation here. This will put our glyph
     * locations in the space in which we draw. Which is later transformed by
     * the transformation matrix that we use. This will transform the
     * glyph positions back to where they were before when drawing, but the
     * glyph shapes will be transformed by the transformation matrix.
     */
    cairo_matrix_transform_point(&scaled_font->mat_inverse, &x, &y);
    offset.advanceOffset = (FLOAT)x;
    /** Y-axis is inverted */
    offset.ascenderOffset = -(FLOAT)y;

    area.top = 0;
    area.bottom = height;
    area.left = 0;
    area.right = width;

    run.glyphCount = 1;
    run.glyphAdvances = &advance;
    run.fontFace = ((cairo_dwrite_font_face_t*)scaled_font->base.font_face)->dwriteface;
    run.fontEmSize = 1.0f;
    run.bidiLevel = 0;
    run.glyphIndices = &index;
    run.isSideways = FALSE;
    run.glyphOffsets = &offset;

    matrix = _cairo_dwrite_matrix_from_matrix(&scaled_font->mat);

    status = _dwrite_draw_glyphs_to_gdi_surface_d2d (to_win32_surface (surface), &matrix, &run,
						     RGB(0, 0, 0), scaled_font, area);
    if (status)
	goto FAIL;

    status = _clone_image_surface (CAIRO_FORMAT_A8, surface, &extents, &image);
    if (status)
	goto FAIL;
    cairo_surface_set_device_offset (&image->base, -x1, -y1);
    _cairo_scaled_glyph_set_surface (scaled_glyph,
				     &scaled_font->base,
				     image);

    cairo_bool_t isColor = FALSE;
    {
	DWRITE_MEASURING_MODE measureMode = _dwrite_scaled_font_mesuaring_mode(scaled_font);
	RefPtr<IDWriteColorGlyphRunEnumerator> colorLayers;
	RefPtr<IDWriteFactory2> factory2;
	DWriteFactory::Instance()->QueryInterface(&factory2);
	factory2->TranslateColorGlyphRun(0, 0, &run, nullptr, measureMode, nullptr, 0, &colorLayers);
	if (colorLayers)
	    isColor = TRUE;
    }
    if (isColor) {
	status = _clone_image_surface (CAIRO_FORMAT_ARGB32, surface, &extents, &image);
	if (status)
	    goto FAIL;
	cairo_surface_set_device_offset (&image->base, -x1, -y1);
	_cairo_scaled_glyph_set_color_surface (scaled_glyph,
					       &scaled_font->base,
					       image);
    }

  FAIL:
    cairo_surface_destroy (surface);

    return status;
}

static cairo_int_status_t
_cairo_dwrite_load_truetype_table(void                 *scaled_font,
				  unsigned long         tag,
				  long                  offset,
				  unsigned char        *buffer,
				  unsigned long        *length)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *face = reinterpret_cast<cairo_dwrite_font_face_t*>(dwritesf->base.font_face);

    const void *data;
    UINT32 size;
    void *tableContext;
    BOOL exists;
    face->dwriteface->TryGetFontTable(be32_to_cpu (tag),
				      &data,
				      &size,
				      &tableContext,
				      &exists);

    if (!exists) {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    if (buffer && *length && (UINT32)offset < size) {
        size = MIN(size - (UINT32)offset, *length);
        memcpy(buffer, (const char*)data + offset, size);
    }
    *length = size;

    if (tableContext) {
	face->dwriteface->ReleaseFontTable(tableContext);
    }
    return (cairo_int_status_t)CAIRO_STATUS_SUCCESS;
}

static cairo_bool_t
_cairo_dwrite_has_color_glyphs(void *scaled_font)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *face = reinterpret_cast<cairo_dwrite_font_face_t*>(dwritesf->base.font_face);
    RefPtr<IDWriteFontFace2> face2;
    HRESULT hr = face->dwriteface->QueryInterface (&face2);
    if (FAILED(hr))
	return FALSE;
    return face2->IsColorFont ();
}

// WIN32 Helper Functions
cairo_font_face_t*
cairo_dwrite_font_face_create_for_dwrite_font_face(IDWriteFontFace* dwrite_font_face)
{
    cairo_dwrite_font_face_t* face = (cairo_dwrite_font_face_t*)malloc(sizeof(cairo_dwrite_font_face_t));
    cairo_font_face_t *font_face = (cairo_font_face_t*)face;

    dwrite_font_face->AddRef();
    
    face->font = NULL;
    face->dwriteface = dwrite_font_face;
    face->rendering_mode = DWRITE_RENDERING_MODE_DEFAULT;
    face->rendering_params = NULL;
    _cairo_font_face_init (&((cairo_dwrite_font_face_t*)font_face)->base, &_cairo_dwrite_font_face_backend);

    return font_face;
}

cairo_public cairo_font_face_t *
cairo_dwrite_font_face_create_for_hfont (HFONT font)
{
    RefPtr<IDWriteGdiInterop> gdiInterop;
    DWriteFactory::Instance()->GetGdiInterop(&gdiInterop);
    if (!gdiInterop)
        return NULL;

    RefPtr<IDWriteFontFace> dwFace;
    HDC hdc = GetDC(NULL);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    HRESULT hr = gdiInterop->CreateFontFaceFromHdc(hdc, &dwFace);
    SelectObject(hdc, oldFont);
    ReleaseDC(NULL, hdc);
    if (FAILED(hr))
        return nullptr;
    return cairo_dwrite_font_face_create_for_dwrite_font_face(dwFace);
}

int
cairo_dwrite_font_face_get_rendering_mode(cairo_font_face_t *font_face)
{
    cairo_dwrite_font_face_t *dwrite_font_face = (cairo_dwrite_font_face_t*)font_face;
    return dwrite_font_face->rendering_mode;
}

void
cairo_dwrite_font_face_set_rendering_mode(cairo_font_face_t *font_face, int mode)
{
    cairo_dwrite_font_face_t *dwrite_font_face = (cairo_dwrite_font_face_t*)font_face;
    if (dwrite_font_face->rendering_mode == (DWRITE_RENDERING_MODE)mode)
	return;
    dwrite_font_face->rendering_mode = (DWRITE_RENDERING_MODE)mode;
    if (dwrite_font_face->rendering_params) {
	dwrite_font_face->rendering_params->Release();
	dwrite_font_face->rendering_params = NULL;
    }
}

static cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_gdi(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       cairo_dwrite_scaled_font_t *scaled_font,
				       const RECT &area)
{
    IDWriteGdiInterop *gdiInterop;
    DWriteFactory::Instance()->GetGdiInterop(&gdiInterop);
    IDWriteBitmapRenderTarget *rt;
    HRESULT rv;

    rv = gdiInterop->CreateBitmapRenderTarget(surface->dc,
					      area.right - area.left,
					      area.bottom - area.top,
					      &rt);

    if (FAILED(rv)) {
	if (rv == E_OUTOFMEMORY) {
	    return (cairo_int_status_t)CAIRO_STATUS_NO_MEMORY;
	} else {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}
    }

#if 0    
    if ((renderingState == cairo_dwrite_scaled_font_t::TEXT_RENDERING_NORMAL ||
         renderingState == cairo_dwrite_scaled_font_t::TEXT_RENDERING_GDI_CLASSIC) &&
        !surface->base.permit_subpixel_antialiasing) {
      renderingState = cairo_dwrite_scaled_font_t::TEXT_RENDERING_NO_CLEARTYPE;
      IDWriteBitmapRenderTarget1* rt1;
      rv = rt->QueryInterface(&rt1);
      
      if (SUCCEEDED(rv) && rt1) {
        rt1->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt1->Release();
      }
    }
#endif

    IDWriteRenderingParams *params = _cairo_dwrite_font_face_get_rendering_params ((cairo_dwrite_font_face_t*)scaled_font->base.font_face);

    /**
     * We set the number of pixels per DIP to 1.0. This is because we always want
     * to draw in device pixels, and not device independent pixels. On high DPI
     * systems this value will be higher than 1.0 and automatically upscale
     * fonts, we don't want this since we do our own upscaling for various reasons.
     */
    rt->SetPixelsPerDip(1.0);

    if (transform) {
	rt->SetCurrentTransform(transform);
    }
    BitBlt(rt->GetMemoryDC(),
	   0, 0,
	   area.right - area.left, area.bottom - area.top,
	   surface->dc,
	   area.left, area.top, 
	   SRCCOPY | NOMIRRORBITMAP);
    DWRITE_MEASURING_MODE measureMode = _dwrite_scaled_font_mesuaring_mode(scaled_font);
    IDWriteColorGlyphRunEnumerator* colorLayers = NULL;
    IDWriteFactory2* factory2;
    DWriteFactory::Instance()->QueryInterface(&factory2);
    factory2->TranslateColorGlyphRun(0, 0, run, nullptr, measureMode, nullptr, 0, &colorLayers);
    factory2->Release();

    if (colorLayers) {
	BOOL hasRun;
	const DWRITE_COLOR_GLYPH_RUN* colorRun;

	while (true) {
	    if (FAILED(colorLayers->MoveNext(&hasRun)) || !hasRun)
		break;
	    if (FAILED(colorLayers->GetCurrentRun(&colorRun)))
		break;

	    if (colorRun->runColor.r || colorRun->runColor.g || colorRun->runColor.b || colorRun->runColor.a)
		color = RGB(colorRun->runColor.r * 255, colorRun->runColor.g * 255, colorRun->runColor.b * 255);
	    HRESULT hr = rt->DrawGlyphRun(0, 0, measureMode, &colorRun->glyphRun, params, color);
	}
	colorLayers->Release();
    } else {
	HRESULT hr = rt->DrawGlyphRun(0, 0, measureMode, run, params, color);
    }
    BitBlt(surface->dc,
	   area.left, area.top,
	   area.right - area.left, area.bottom - area.top,
	   rt->GetMemoryDC(),
	   0, 0, 
	   SRCCOPY | NOMIRRORBITMAP);
    params->Release();
    rt->Release();
    gdiInterop->Release();
    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_d2d(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       cairo_dwrite_scaled_font_t *scaled_font,
				       const RECT &area)
{
    HRESULT rv;
    ID2D1DCRenderTarget *rt = D2DFactory::RenderTarget();

    // XXX don't we need to set RenderingParams on this RenderTarget?

    rv = rt->BindDC(surface->dc, &area);

    if (FAILED(rv))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    D2D1_COLOR_F defalutColor = D2D1::ColorF(GetRValue(color), GetGValue(color), GetBValue(color), 1);

    if (transform) {
	rt->SetTransform(D2D1::Matrix3x2F(transform->m11,
					  transform->m12,
					  transform->m21,
					  transform->m22,
					  transform->dx,
					  transform->dy));
    }
    rt->BeginDraw();

    DWRITE_MEASURING_MODE measureMode = _dwrite_scaled_font_mesuaring_mode(scaled_font);
    RefPtr<IDWriteColorGlyphRunEnumerator> colorLayers;
    RefPtr<IDWriteFactory2> factory2;
    DWriteFactory::Instance()->QueryInterface(&factory2);
    if (factory2)
	factory2->TranslateColorGlyphRun(0, 0, run, nullptr, measureMode, nullptr, 0, &colorLayers);

    if (colorLayers) {
	BOOL hasRun;
	const DWRITE_COLOR_GLYPH_RUN* colorRun;

	while (true) {
	    if (FAILED(colorLayers->MoveNext(&hasRun)) || !hasRun)
		break;
	    if (FAILED(colorLayers->GetCurrentRun(&colorRun)))
		break;

	    D2D1_COLOR_F color;
	    if (colorRun->runColor.r || colorRun->runColor.g || colorRun->runColor.b || colorRun->runColor.a)
		color = colorRun->runColor;
	    else
		color = defalutColor;

	    RefPtr<ID2D1SolidColorBrush> brush;
	    rv = rt->CreateSolidColorBrush(color, &brush);
	    if (FAILED(rv))
		break;
	    rt->DrawGlyphRun(D2D1::Point2F(0, 0), &colorRun->glyphRun, brush, measureMode);
	}
    } else {
	RefPtr<ID2D1SolidColorBrush> brush;
	rv = rt->CreateSolidColorBrush(defalutColor, &brush);
	if (SUCCEEDED(rv))
	    rt->DrawGlyphRun(D2D1::Point2F(0, 0), run, brush, measureMode);
    }

    rt->EndDraw();
    if (transform)
	rt->SetTransform(D2D1::Matrix3x2F::Identity());

    if (FAILED(rv))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    return CAIRO_INT_STATUS_SUCCESS;
}

/* Surface helper function */
cairo_int_status_t
_cairo_dwrite_show_glyphs_on_surface(void			*surface,
				    cairo_operator_t	 op,
				    const cairo_pattern_t	*source,
				    cairo_glyph_t		*glyphs,
				    int			 num_glyphs,
				    cairo_scaled_font_t	*scaled_font)
{
    // TODO: Check font & surface for types.
    cairo_dwrite_scaled_font_t *dwritesf = reinterpret_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *dwriteff = reinterpret_cast<cairo_dwrite_font_face_t*>(scaled_font->font_face);
    cairo_win32_surface_t *dst = reinterpret_cast<cairo_win32_surface_t*>(surface);
    cairo_int_status_t status;
    /* We can only handle dwrite fonts */
    if (cairo_scaled_font_get_type (scaled_font) != CAIRO_FONT_TYPE_DWRITE)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    /* We can only handle opaque solid color sources */
    if (!_cairo_pattern_is_opaque_solid(source))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    /* We can only handle operator SOURCE or OVER with the destination
     * having no alpha */
    if (op != CAIRO_OPERATOR_SOURCE && op != CAIRO_OPERATOR_OVER)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    /* It is vital that dx values for dxy_buf are calculated from the delta of
     * _logical_ x coordinates (not user x coordinates) or else the sum of all
     * previous dx values may start to diverge from the current glyph's x
     * coordinate due to accumulated rounding error. As a result strings could
     * be painted shorter or longer than expected. */

    AutoDWriteGlyphRun run;
    run.allocate(num_glyphs);

    UINT16 *indices = const_cast<UINT16*>(run.glyphIndices);
    FLOAT *advances = const_cast<FLOAT*>(run.glyphAdvances);
    DWRITE_GLYPH_OFFSET *offsets = const_cast<DWRITE_GLYPH_OFFSET*>(run.glyphOffsets);

    BOOL transform = FALSE;
    /* Needed to calculate bounding box for efficient blitting */
    INT32 smallestX = INT_MAX;
    INT32 largestX = 0;
    INT32 smallestY = INT_MAX;
    INT32 largestY = 0;
    for (int i = 0; i < num_glyphs; i++) {
	if (glyphs[i].x < smallestX) {
	    smallestX = (INT32)glyphs[i].x;
	}
	if (glyphs[i].x > largestX) {
	    largestX = (INT32)glyphs[i].x;
	}
	if (glyphs[i].y < smallestY) {
	    smallestY = (INT32)glyphs[i].y;
	}
	if (glyphs[i].y > largestY) {
	    largestY = (INT32)glyphs[i].y;
	}
    }
    /**
     * Here we try to get a rough estimate of the area that this glyph run will
     * cover on the surface. Since we use GDI interop to draw we will be copying
     * data around the size of the area of the surface that we map. We will want
     * to map an area as small as possible to prevent large surfaces to be
     * copied around. We take the X/Y-size of the font as margin on the left/top
     * twice the X/Y-size of the font as margin on the right/bottom.
     * This should always cover the entire area where the glyphs are.
     */
    RECT fontArea;
    fontArea.left = (INT32)(smallestX - scaled_font->font_matrix.xx);
    fontArea.right = (INT32)(largestX + scaled_font->font_matrix.xx * 2);
    fontArea.top = (INT32)(smallestY - scaled_font->font_matrix.yy);
    fontArea.bottom = (INT32)(largestY + scaled_font->font_matrix.yy * 2);
    if (fontArea.left < 0)
	fontArea.left = 0;
    if (fontArea.top < 0)
	fontArea.top = 0;
    if (fontArea.bottom > dst->extents.height) {
	fontArea.bottom = dst->extents.height;
    }
    if (fontArea.right > dst->extents.width) {
	fontArea.right = dst->extents.width;
    }
    if (fontArea.right <= fontArea.left ||
	fontArea.bottom <= fontArea.top) {
	return CAIRO_INT_STATUS_SUCCESS;
    }
    if (fontArea.right > dst->extents.width) {
	fontArea.right = dst->extents.width;
    }
    if (fontArea.bottom > dst->extents.height) {
	fontArea.bottom = dst->extents.height;
    }

    run.bidiLevel = 0;
    run.fontFace = dwriteff->dwriteface;
    run.isSideways = FALSE;
    if (dwritesf->mat.xy == 0 && dwritesf->mat.yx == 0 &&
	dwritesf->mat.xx == scaled_font->font_matrix.xx && 
	dwritesf->mat.yy == scaled_font->font_matrix.yy) {

	for (int i = 0; i < num_glyphs; i++) {
	    indices[i] = (WORD) glyphs[i].index;
	    // Since we will multiply by our ctm matrix later for rotation effects
	    // and such, adjust positions by the inverse matrix now.
	    offsets[i].ascenderOffset = (FLOAT)(fontArea.top - glyphs[i].y);
	    offsets[i].advanceOffset = (FLOAT)(glyphs[i].x - fontArea.left);
	    advances[i] = 0.0;
	}
	run.fontEmSize = (FLOAT)scaled_font->font_matrix.yy;
    } else {
	transform = TRUE;
        // See comment about EPSILON in _cairo_dwrite_glyph_run_from_glyphs
        const double EPSILON = 0.0001;
	for (int i = 0; i < num_glyphs; i++) {
	    indices[i] = (WORD) glyphs[i].index;
	    double x = glyphs[i].x - fontArea.left + EPSILON;
	    double y = glyphs[i].y - fontArea.top;
	    cairo_matrix_transform_point(&dwritesf->mat_inverse, &x, &y);
	    /**
	     * Since we will multiply by our ctm matrix later for rotation effects
	     * and such, adjust positions by the inverse matrix now. The Y-axis
	     * is inverted so the offset becomes negative.
	     */
	    offsets[i].ascenderOffset = -(FLOAT)y;
	    offsets[i].advanceOffset = (FLOAT)x;
	    advances[i] = 0.0;
	}
	run.fontEmSize = 1.0f;
    }
    
    cairo_solid_pattern_t *solid_pattern = (cairo_solid_pattern_t *)source;
    COLORREF color = RGB(((int)solid_pattern->color.red_short) >> 8,
		((int)solid_pattern->color.green_short) >> 8,
		((int)solid_pattern->color.blue_short) >> 8);

    DWRITE_MATRIX matrix = _cairo_dwrite_matrix_from_matrix(&dwritesf->mat);

    DWRITE_MATRIX *mat;
    if (transform) {
	mat = &matrix;
    } else {
	mat = NULL;
    }

    RECT area;
    area.left = dst->extents.x;
    area.top = dst->extents.y;
    area.right = area.left + dst->extents.width;
    area.bottom = area.top + dst->extents.height;

#ifdef CAIRO_TRY_D2D_TO_GDI
    status = _dwrite_draw_glyphs_to_gdi_surface_d2d(dst,
						    mat,
						    &run,
						    color,
						    fontArea);

    if (status == (cairo_status_t)CAIRO_INT_STATUS_UNSUPPORTED) {
#endif
	status = _dwrite_draw_glyphs_to_gdi_surface_gdi(dst,
							mat,
							&run,
							color,
							dwritesf,
							fontArea);

#ifdef CAIRO_TRY_D2D_TO_GDI
    }
#endif

    return status;
}

static cairo_bool_t
_name_tables_match (cairo_scaled_font_t *font1,
                    cairo_scaled_font_t *font2)
{
    unsigned long size1;
    unsigned long size2;
    cairo_int_status_t status1;
    cairo_int_status_t status2;
    unsigned char *buffer1;
    unsigned char *buffer2;
    cairo_bool_t result = false;

    if (!font1->backend || !font2->backend ||
        !font1->backend->load_truetype_table ||
        !font2->backend->load_truetype_table)
        return false;

    status1 = font1->backend->load_truetype_table (font1,
                                                   TT_TAG_name, 0, NULL, &size1);
    status2 = font2->backend->load_truetype_table (font2,
                                                   TT_TAG_name, 0, NULL, &size2);
    if (status1 || status2)
        return false;
    if (size1 != size2)
        return false;

    buffer1 = (unsigned char*)malloc (size1);
    buffer2 = (unsigned char*)malloc (size2);

    if (buffer1 && buffer2) {
        status1 = font1->backend->load_truetype_table (font1,
                                                       TT_TAG_name, 0, buffer1, &size1);
        status2 = font2->backend->load_truetype_table (font2,
                                                       TT_TAG_name, 0, buffer2, &size2);
        if (!status1 && !status2) {
            result = memcmp (buffer1, buffer2, size1) == 0;
        }
    }

    free (buffer1);
    free (buffer2);
    return result;
}

// Helper for _cairo_win32_printing_surface_show_glyphs to create a win32 equivalent
// of a dwrite scaled_font so that we can print using ExtTextOut instead of drawing
// paths or blitting glyph bitmaps.
cairo_int_status_t
_cairo_dwrite_scaled_font_create_win32_scaled_font (cairo_scaled_font_t *scaled_font,
                                                    cairo_scaled_font_t **new_font)
{
    if (cairo_scaled_font_get_type (scaled_font) != CAIRO_FONT_TYPE_DWRITE) {
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    cairo_font_face_t *face = cairo_scaled_font_get_font_face (scaled_font);
    cairo_dwrite_font_face_t *dwface = reinterpret_cast<cairo_dwrite_font_face_t*>(face);

    RefPtr<IDWriteGdiInterop> gdiInterop;
    DWriteFactory::Instance()->GetGdiInterop(&gdiInterop);
    if (!gdiInterop) {
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    LOGFONTW logfont;
    if (FAILED(gdiInterop->ConvertFontFaceToLOGFONT (dwface->dwriteface, &logfont))) {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }
    // DW must have been using an outline font, so we want GDI to use the same,
    // even if there's also a bitmap face available
    logfont.lfOutPrecision = OUT_OUTLINE_PRECIS;

    cairo_font_face_t *win32_face = cairo_win32_font_face_create_for_logfontw (&logfont);
    if (!win32_face) {
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    cairo_matrix_t font_matrix;
    cairo_scaled_font_get_font_matrix (scaled_font, &font_matrix);

    cairo_matrix_t ctm;
    cairo_scaled_font_get_ctm (scaled_font, &ctm);

    cairo_font_options_t options;
    cairo_scaled_font_get_font_options (scaled_font, &options);

    cairo_scaled_font_t *font = cairo_scaled_font_create (win32_face,
			                                  &font_matrix,
			                                  &ctm,
			                                  &options);
    cairo_font_face_destroy (win32_face);

    if (!font) {
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    if (!_name_tables_match (font, scaled_font)) {
        // If the font name tables aren't equal, then GDI may have failed to
        // find the right font and substituted a different font.
        cairo_scaled_font_destroy (font);
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    *new_font = font;
    return CAIRO_INT_STATUS_SUCCESS;
}
