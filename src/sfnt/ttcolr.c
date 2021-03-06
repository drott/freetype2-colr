/****************************************************************************
 *
 * ttcolr.c
 *
 *   TrueType and OpenType colored glyph layer support (body).
 *
 * Copyright (C) 2018-2020 by
 * David Turner, Robert Wilhelm, Dominik Röttsches and Werner Lemberg.
 *
 * Originally written by Shao Yu Zhang <shaozhang@fb.com>.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 */


  /**************************************************************************
   *
   * `COLR' table specification:
   *
   *   https://www.microsoft.com/typography/otspec/colr.htm
   *
   */


#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftstream.h>
#include <freetype/tttags.h>
#include <freetype/ftcolor.h>


#ifdef TT_CONFIG_OPTION_COLOR_LAYERS

#include "ttcolr.h"


  /* NOTE: These are the table sizes calculated through the specs. */
#define BASE_GLYPH_SIZE            6
#define BASE_GLYPH_V1_SIZE         6
#define LAYER_V1_RECORD_SIZE       6
#define COLOR_STOP_SIZE            6
#define LAYER_SIZE                 4
#define COLR_HEADER_SIZE          14


  typedef struct BaseGlyphRecord_
  {
    FT_UShort  gid;
    FT_UShort  first_layer_index;
    FT_UShort  num_layers;

  } BaseGlyphRecord;

  typedef struct BaseGlyphV1Record_
  {
    FT_UShort gid;
    /* offet to parent BaseGlyphV1Array */
    FT_ULong layer_array_offset;
  } BaseGlyphV1Record;

  typedef struct Colr_
  {
    FT_UShort  version;
    FT_UShort  num_base_glyphs;
    FT_UShort  num_layers;

    FT_Byte*  base_glyphs;
    FT_Byte*  layers;

    FT_UShort num_base_glyphs_v1;
    FT_Byte*  base_glyphs_v1;

    /* The memory which backs up the `COLR' table. */
    void*     table;
    FT_ULong  table_size;

  } Colr;


  /**************************************************************************
   *
   * The macro FT_COMPONENT is used in trace mode.  It is an implicit
   * parameter of the FT_TRACE() and FT_ERROR() macros, used to print/log
   * messages during execution.
   */
#undef  FT_COMPONENT
#define FT_COMPONENT  ttcolr


  FT_LOCAL_DEF( FT_Error )
  tt_face_load_colr( TT_Face    face,
                     FT_Stream  stream )
  {
    FT_Error   error;
    FT_Memory  memory = face->root.memory;

    FT_Byte *table = NULL;
    FT_Byte *p = NULL;

    Colr*  colr = NULL;

    FT_ULong base_glyph_offset, layer_offset, base_glyphs_v1_offset,
        num_base_glyphs_v1;
    FT_ULong  table_size;


    /* `COLR' always needs `CPAL' */
    if ( !face->cpal )
      return FT_THROW( Invalid_File_Format );

    error = face->goto_table( face, TTAG_COLR, stream, &table_size );
    if ( error )
      goto NoColr;

    if ( table_size < COLR_HEADER_SIZE )
      goto InvalidTable;

    if ( FT_FRAME_EXTRACT( table_size, table ) )
      goto NoColr;

    p = table;

    if ( FT_NEW( colr ) )
      goto NoColr;

    colr->version = FT_NEXT_USHORT( p );
    if ( colr->version != 0 && colr->version != 1 )
      goto InvalidTable;

    colr->num_base_glyphs = FT_NEXT_USHORT( p );
    base_glyph_offset     = FT_NEXT_ULONG( p );

    if ( base_glyph_offset >= table_size )
      goto InvalidTable;
    if ( colr->num_base_glyphs * BASE_GLYPH_SIZE >
           table_size - base_glyph_offset )
      goto InvalidTable;

    layer_offset     = FT_NEXT_ULONG( p );
    colr->num_layers = FT_NEXT_USHORT( p );

    if ( layer_offset >= table_size )
      goto InvalidTable;
    if ( colr->num_layers * LAYER_SIZE > table_size - layer_offset )
      goto InvalidTable;

    if ( colr->version == 1 ) {
      base_glyphs_v1_offset = FT_NEXT_ULONG( p );

      if ( base_glyphs_v1_offset >= table_size )
        goto InvalidTable;

      p = (FT_Byte*)( table + base_glyphs_v1_offset );
      num_base_glyphs_v1 = FT_PEEK_ULONG( p );

      if ( num_base_glyphs_v1 * BASE_GLYPH_V1_SIZE >
           table_size - base_glyphs_v1_offset )
      {
        goto InvalidTable;
      }
      colr->num_base_glyphs_v1 = num_base_glyphs_v1;
      colr->base_glyphs_v1     = p;
    }

    colr->base_glyphs = (FT_Byte*)( table + base_glyph_offset );
    colr->layers      = (FT_Byte*)( table + layer_offset      );
    colr->table       = table;
    colr->table_size  = table_size;

    face->colr = colr;

    return FT_Err_Ok;

  InvalidTable:
    error = FT_THROW( Invalid_Table );

  NoColr:
    FT_FRAME_RELEASE( table );
    FT_FREE( colr );

    return error;
  }


  FT_LOCAL_DEF( void )
  tt_face_free_colr( TT_Face  face )
  {
    FT_Stream  stream = face->root.stream;
    FT_Memory  memory = face->root.memory;

    Colr*  colr = (Colr*)face->colr;


    if ( colr )
    {
      FT_FRAME_RELEASE( colr->table );
      FT_FREE( colr );
    }
  }


  static FT_Bool
  find_base_glyph_record( FT_Byte*          base_glyph_begin,
                          FT_Int            num_base_glyph,
                          FT_UInt           glyph_id,
                          BaseGlyphRecord*  record )
  {
    FT_Int  min = 0;
    FT_Int  max = num_base_glyph - 1;


    while ( min <= max )
    {
      FT_Int    mid = min + ( max - min ) / 2;
      FT_Byte*  p   = base_glyph_begin + mid * BASE_GLYPH_SIZE;

      FT_UShort  gid = FT_NEXT_USHORT( p );


      if ( gid < glyph_id )
        min = mid + 1;
      else if (gid > glyph_id )
        max = mid - 1;
      else
      {
        record->gid               = gid;
        record->first_layer_index = FT_NEXT_USHORT( p );
        record->num_layers        = FT_NEXT_USHORT( p );

        return 1;
      }
    }

    return 0;
  }


  FT_LOCAL_DEF( FT_Bool )
  tt_face_get_colr_layer( TT_Face            face,
                          FT_UInt            base_glyph,
                          FT_UInt           *aglyph_index,
                          FT_UInt           *acolor_index,
                          FT_LayerIterator*  iterator )
  {
    Colr*            colr = (Colr*)face->colr;
    BaseGlyphRecord  glyph_record;


    if ( !colr )
      return 0;

    if ( !iterator->p )
    {
      FT_ULong  offset;


      /* first call to function */
      iterator->layer = 0;

      if ( !find_base_glyph_record( colr->base_glyphs,
                                    colr->num_base_glyphs,
                                    base_glyph,
                                    &glyph_record ) )
        return 0;

      if ( glyph_record.num_layers )
        iterator->num_layers = glyph_record.num_layers;
      else
        return 0;

      offset = LAYER_SIZE * glyph_record.first_layer_index;
      if ( offset + LAYER_SIZE * glyph_record.num_layers > colr->table_size )
        return 0;

      iterator->p = colr->layers + offset;
    }

    if ( iterator->layer >= iterator->num_layers )
      return 0;

    *aglyph_index = FT_NEXT_USHORT( iterator->p );
    *acolor_index = FT_NEXT_USHORT( iterator->p );

    if ( *aglyph_index >= (FT_UInt)( FT_FACE( face )->num_glyphs )   ||
         ( *acolor_index != 0xFFFF                                 &&
           *acolor_index >= face->palette_data.num_palette_entries ) )
      return 0;

    iterator->layer++;

    return 1;
  }

  static FT_Bool
  read_color_line ( Colr *        colr,
                    FT_Byte *     paint_base,
                    FT_ULong      colorline_offset,
                    FT_ColorLine *colorline )
  {
    FT_Byte *      p = (FT_Byte *)( paint_base + colorline_offset );
    FT_PaintExtend paint_extend;
    /* TODO: Check pointer limits. */

    paint_extend = FT_NEXT_USHORT ( p );
    if ( paint_extend > COLR_PAINT_EXTEND_REFLECT )
      return 0;

    colorline->extend = paint_extend;

    colorline->color_stop_iterator.num_color_stops    = FT_NEXT_USHORT ( p );
    colorline->color_stop_iterator.p                  = p;
    colorline->color_stop_iterator.current_color_stop = 0;

    return 1;
  }

  static FT_Bool
  read_affine ( Colr *     colr,
                FT_Byte *  paint_base,
                FT_ULong   affine_offset,
                FT_Matrix *affine )
  {
    FT_Byte *p = (FT_Byte *)( paint_base + affine_offset );
    /* TODO: Check pointer limits against colr->table etc. */

    affine->xx = FT_NEXT_LONG ( p );
    FT_NEXT_ULONG ( p ); /* drop varIdx */
    affine->xy = FT_NEXT_LONG ( p );
    FT_NEXT_ULONG ( p ); /* drop varIdx */
    affine->yx = FT_NEXT_LONG ( p );
    FT_NEXT_ULONG ( p ); /* drop varIdx */
    affine->yy = FT_NEXT_LONG ( p );
    FT_NEXT_ULONG ( p ); /* drop varIdx */

    return 1;
  }

  static FT_Bool
  read_paint ( Colr *         colr,
               FT_Byte *      layer_v1_array,
               FT_ULong       paint_offset,
               FT_COLR_Paint *apaint )
  {
    FT_Byte *p, *paint_base;

    p          = layer_v1_array + paint_offset;
    paint_base = p;

    apaint->format = FT_NEXT_USHORT ( p );

    if ( apaint->format >= COLR_PAINT_FORMAT_MAX )
      return 0;

    if ( apaint->format == COLR_PAINTFORMAT_SOLID )
    {
      apaint->u.solid.color.palette_index = FT_NEXT_USHORT ( p );
      apaint->u.solid.color.alpha = FT_NEXT_USHORT ( p );
      /* skip VarIdx */
      FT_NEXT_ULONG ( p );
    }
    else if ( apaint->format == COLR_PAINTFORMAT_LINEAR_GRADIENT )
    {
      FT_ULong color_line_offset = 0;
      color_line_offset = FT_NEXT_ULONG ( p );
      if ( !read_color_line ( colr,
                              paint_base,
                              color_line_offset,
                              &apaint->u.linear_gradient.colorline ) )
        return 0;
      apaint->u.linear_gradient.p0.x = FT_NEXT_SHORT ( p );
      /* skip VarIdx */
      FT_NEXT_ULONG ( p );
      apaint->u.linear_gradient.p0.y = FT_NEXT_SHORT ( p );
      FT_NEXT_ULONG ( p );
      apaint->u.linear_gradient.p1.x = FT_NEXT_SHORT ( p );
      FT_NEXT_ULONG ( p );
      apaint->u.linear_gradient.p1.y = FT_NEXT_SHORT ( p );
      FT_NEXT_ULONG ( p );
      apaint->u.linear_gradient.p2.x = FT_NEXT_SHORT ( p );
      FT_NEXT_ULONG ( p );
      apaint->u.linear_gradient.p2.y = FT_NEXT_SHORT ( p );
      FT_NEXT_ULONG ( p );
    } else if ( apaint->format == COLR_PAINTFORMAT_RADIAL_GRADIENT )
    {
      FT_ULong color_line_offset = 0;
      FT_ULong affine_offset = 0;

      color_line_offset = FT_NEXT_ULONG ( p );
      if ( !read_color_line ( colr,
                              paint_base,
                              color_line_offset,
                              &apaint->u.linear_gradient.colorline ) )
        return 0;

      apaint->u.radial_gradient.c0.x = FT_NEXT_SHORT ( p );
      /* skip VarIdx */
      FT_NEXT_ULONG ( p );
      apaint->u.radial_gradient.c0.y = FT_NEXT_SHORT ( p );
      /* skip VarIdx */
      FT_NEXT_ULONG ( p );

      apaint->u.radial_gradient.r0 = FT_NEXT_USHORT ( p );
      FT_NEXT_ULONG ( p );

      apaint->u.radial_gradient.c1.x = FT_NEXT_SHORT ( p );
      /* skip VarIdx */
      FT_NEXT_ULONG ( p );
      apaint->u.radial_gradient.c1.y = FT_NEXT_SHORT ( p );
      /* skip VarIdx */
      FT_NEXT_ULONG ( p );

      apaint->u.radial_gradient.r1 = FT_NEXT_USHORT ( p );
      FT_NEXT_ULONG ( p );

      affine_offset = FT_NEXT_ULONG ( p );

      if ( !affine_offset )
        return 1;

      /* TODO: Is there something like a const FT_Matrix_Unity? */
      apaint->u.radial_gradient.affine.xx = 1;
      apaint->u.radial_gradient.affine.xy = 0;
      apaint->u.radial_gradient.affine.yx = 1;
      apaint->u.radial_gradient.affine.yy = 0;

      if ( !read_affine ( colr,
                          paint_base,
                          affine_offset,
                          &apaint->u.radial_gradient.affine ) )
        return 0;
    }

    return 1;
  }

  static FT_Bool
  find_base_glyph_v1_record ( FT_Byte *          base_glyph_begin,
                              FT_Int             num_base_glyph,
                              FT_UInt            glyph_id,
                              BaseGlyphV1Record *record )
  {
    FT_Int  min = 0;
    FT_Int  max = num_base_glyph - 1;


    while ( min <= max )
    {
      FT_Int    mid = min + ( max - min ) / 2;
      /* base_glyph_begin is the beginning of the BaseGlyphV1Array, skip the
       * array length by adding 4 to start binary search in layers. */
      FT_Byte * p   = base_glyph_begin + 4 + mid * BASE_GLYPH_V1_SIZE;

      FT_UShort  gid = FT_NEXT_USHORT( p );

      if ( gid < glyph_id )
        min = mid + 1;
      else if (gid > glyph_id )
        max = mid - 1;
      else
      {
        record->gid                = gid;
        record->layer_array_offset = FT_NEXT_ULONG ( p );
        return 1;
      }
    }

    return 0;
  }

  FT_LOCAL_DEF ( FT_Bool )
  tt_face_get_colr_layer_gradients ( TT_Face           face,
                                     FT_UInt           base_glyph,
                                     FT_UInt *         aglyph_index,
                                     FT_COLR_Paint *   paint,
                                     FT_LayerIterator *iterator )
  {
    Colr* colr = (Colr*)face->colr;
    BaseGlyphV1Record base_glyph_v1_record;
    FT_Byte *         p, *layer_v1_array;
    FT_UInt gid;

    if ( colr->version < 1 || !colr->num_base_glyphs_v1 ||
         !colr->base_glyphs_v1 )
      return 0;

    if ( !iterator->p )
    {
      iterator->layer = 0;
      if ( !find_base_glyph_v1_record ( colr->base_glyphs_v1,
                                        colr->num_base_glyphs_v1,
                                        base_glyph,
                                        &base_glyph_v1_record ) )
        return 0;

      /* Try to find layer size to configure iterator */
      if ( !base_glyph_v1_record.layer_array_offset ||
           base_glyph_v1_record.layer_array_offset > colr->table_size )
        return 0;

      p                    = (FT_Byte *)( colr->base_glyphs_v1 +
                       base_glyph_v1_record.layer_array_offset );
      iterator->num_layers = FT_NEXT_ULONG ( p );

      if ( p > (FT_Byte *)( colr->table + colr->table_size ) )
        return 0;

      iterator->p = p;
    }

    if ( iterator->layer >= iterator->num_layers )
      return 0;

    /* We have an iterator pointing at a LayerV1Record */
    p = iterator->p;


    /* reverse to layer_v1_array, TODO */
    /* TODO: More checks on this one. */
    layer_v1_array = p - iterator->layer * LAYER_V1_RECORD_SIZE - 4 /* array size */;

    gid = FT_NEXT_USHORT(p);

    if ( gid > ( FT_UInt ) ( FT_FACE ( face )->num_glyphs ) )
      return 0;

    if ( !read_paint ( colr, layer_v1_array, FT_NEXT_ULONG ( p ), paint ) )
      return 0;

    *aglyph_index = gid;
    iterator->p = p;

    iterator->layer++;

    return 1;
  }

  FT_LOCAL_DEF ( FT_Bool )
  tt_face_get_colorline_stops ( TT_Face               face,
                                FT_ColorStop *        color_stop,
                                FT_ColorStopIterator *iterator )
  {
    Colr* colr = (Colr*)face->colr;
    FT_Byte *p;

    if ( iterator->current_color_stop >= iterator->num_color_stops )
      return 0;

    if ( iterator->p +
             ( ( iterator->num_color_stops - iterator->current_color_stop ) *
               COLOR_STOP_SIZE ) >
         (FT_Byte *)( colr->table + colr->table_size ) )
      return 0;

    /* Iterator points at first ColorStop of ColorLine */
    p = iterator->p;

    color_stop->stop_offset         = FT_NEXT_USHORT ( p );
    FT_NEXT_ULONG ( p ); /* skip varIdx */
    color_stop->color.palette_index = FT_NEXT_USHORT ( p );
    color_stop->color.alpha         = FT_NEXT_USHORT ( p );
    FT_NEXT_ULONG ( p ); /* skip varIdx */

    iterator->p = p;
    iterator->current_color_stop++;

    return 1;
  }

  FT_LOCAL_DEF( FT_Error )
  tt_face_colr_blend_layer( TT_Face       face,
                            FT_UInt       color_index,
                            FT_GlyphSlot  dstSlot,
                            FT_GlyphSlot  srcSlot )
  {
    FT_Error  error;

    FT_UInt  x, y;
    FT_Byte  b, g, r, alpha;

    FT_ULong  size;
    FT_Byte*  src;
    FT_Byte*  dst;


    if ( !dstSlot->bitmap.buffer )
    {
      /* Initialize destination of color bitmap */
      /* with the size of first component.      */
      dstSlot->bitmap_left = srcSlot->bitmap_left;
      dstSlot->bitmap_top  = srcSlot->bitmap_top;

      dstSlot->bitmap.width      = srcSlot->bitmap.width;
      dstSlot->bitmap.rows       = srcSlot->bitmap.rows;
      dstSlot->bitmap.pixel_mode = FT_PIXEL_MODE_BGRA;
      dstSlot->bitmap.pitch      = (int)dstSlot->bitmap.width * 4;
      dstSlot->bitmap.num_grays  = 256;

      size = dstSlot->bitmap.rows * (unsigned int)dstSlot->bitmap.pitch;

      error = ft_glyphslot_alloc_bitmap( dstSlot, size );
      if ( error )
        return error;

      FT_MEM_ZERO( dstSlot->bitmap.buffer, size );
    }
    else
    {
      /* Resize destination if needed such that new component fits. */
      FT_Int  x_min, x_max, y_min, y_max;


      x_min = FT_MIN( dstSlot->bitmap_left, srcSlot->bitmap_left );
      x_max = FT_MAX( dstSlot->bitmap_left + (FT_Int)dstSlot->bitmap.width,
                      srcSlot->bitmap_left + (FT_Int)srcSlot->bitmap.width );

      y_min = FT_MIN( dstSlot->bitmap_top - (FT_Int)dstSlot->bitmap.rows,
                      srcSlot->bitmap_top - (FT_Int)srcSlot->bitmap.rows );
      y_max = FT_MAX( dstSlot->bitmap_top, srcSlot->bitmap_top );

      if ( x_min != dstSlot->bitmap_left                                 ||
           x_max != dstSlot->bitmap_left + (FT_Int)dstSlot->bitmap.width ||
           y_min != dstSlot->bitmap_top - (FT_Int)dstSlot->bitmap.rows   ||
           y_max != dstSlot->bitmap_top                                  )
      {
        FT_Memory  memory = face->root.memory;

        FT_UInt  width = (FT_UInt)( x_max - x_min );
        FT_UInt  rows  = (FT_UInt)( y_max - y_min );
        FT_UInt  pitch = width * 4;

        FT_Byte*  buf = NULL;
        FT_Byte*  p;
        FT_Byte*  q;


        size  = rows * pitch;
        if ( FT_ALLOC( buf, size ) )
          return error;

        p = dstSlot->bitmap.buffer;
        q = buf +
            (int)pitch * ( y_max - dstSlot->bitmap_top ) +
            4 * ( dstSlot->bitmap_left - x_min );

        for ( y = 0; y < dstSlot->bitmap.rows; y++ )
        {
          FT_MEM_COPY( q, p, dstSlot->bitmap.width * 4 );

          p += dstSlot->bitmap.pitch;
          q += pitch;
        }

        ft_glyphslot_set_bitmap( dstSlot, buf );

        dstSlot->bitmap_top  = y_max;
        dstSlot->bitmap_left = x_min;

        dstSlot->bitmap.width = width;
        dstSlot->bitmap.rows  = rows;
        dstSlot->bitmap.pitch = (int)pitch;

        dstSlot->internal->flags |= FT_GLYPH_OWN_BITMAP;
        dstSlot->format           = FT_GLYPH_FORMAT_BITMAP;
      }
    }

    if ( color_index == 0xFFFF )
    {
      if ( face->have_foreground_color )
      {
        b     = face->foreground_color.blue;
        g     = face->foreground_color.green;
        r     = face->foreground_color.red;
        alpha = face->foreground_color.alpha;
      }
      else
      {
        if ( face->palette_data.palette_flags                          &&
             ( face->palette_data.palette_flags[face->palette_index] &
                 FT_PALETTE_FOR_DARK_BACKGROUND                      ) )
        {
          /* white opaque */
          b     = 0xFF;
          g     = 0xFF;
          r     = 0xFF;
          alpha = 0xFF;
        }
        else
        {
          /* black opaque */
          b     = 0x00;
          g     = 0x00;
          r     = 0x00;
          alpha = 0xFF;
        }
      }
    }
    else
    {
      b     = face->palette[color_index].blue;
      g     = face->palette[color_index].green;
      r     = face->palette[color_index].red;
      alpha = face->palette[color_index].alpha;
    }

    /* XXX Convert if srcSlot.bitmap is not grey? */
    src = srcSlot->bitmap.buffer;
    dst = dstSlot->bitmap.buffer +
          dstSlot->bitmap.pitch * ( dstSlot->bitmap_top - srcSlot->bitmap_top ) +
          4 * ( srcSlot->bitmap_left - dstSlot->bitmap_left );

    for ( y = 0; y < srcSlot->bitmap.rows; y++ )
    {
      for ( x = 0; x < srcSlot->bitmap.width; x++ )
      {
        int  aa = src[x];
        int  fa = alpha * aa / 255;

        int  fb = b * fa / 255;
        int  fg = g * fa / 255;
        int  fr = r * fa / 255;

        int  ba2 = 255 - fa;

        int  bb = dst[4 * x + 0];
        int  bg = dst[4 * x + 1];
        int  br = dst[4 * x + 2];
        int  ba = dst[4 * x + 3];


        dst[4 * x + 0] = (FT_Byte)( bb * ba2 / 255 + fb );
        dst[4 * x + 1] = (FT_Byte)( bg * ba2 / 255 + fg );
        dst[4 * x + 2] = (FT_Byte)( br * ba2 / 255 + fr );
        dst[4 * x + 3] = (FT_Byte)( ba * ba2 / 255 + fa );
      }

      src += srcSlot->bitmap.pitch;
      dst += dstSlot->bitmap.pitch;
    }

    return FT_Err_Ok;
  }

#else /* !TT_CONFIG_OPTION_COLOR_LAYERS */

  /* ANSI C doesn't like empty source files */
  typedef int  _tt_colr_dummy;

#endif /* !TT_CONFIG_OPTION_COLOR_LAYERS */

/* EOF */
