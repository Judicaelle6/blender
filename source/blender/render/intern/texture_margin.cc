/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

/** \file
 * \ingroup render
 */

#include "BLI_assert.h"
#include "BLI_math_geom.h"
#include "BLI_math_vec_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "BKE_DerivedMesh.h"
#include "BKE_mesh.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "MEM_guardedalloc.h"

#include "zbuf.h"  // for rasterizer

#include "RE_texture_margin.h"

#include <algorithm>
#include <cmath>
#include <valarray>

namespace blender::render::texturemargin {

/**
 * The map class contains both a pixel map which maps out polygon indices for all UV-polygons and
 * adjacency tables.
 */
class TextureMarginMap {
  static const int directions[8][2];
  static const int distances[8];

  /** Maps UV-edges to their corresponding UV-edge. */
  Vector<int> loop_adjacency_map_;
  /** Maps UV-edges to their corresponding polygon. */
  Vector<int> loop_to_poly_map_;

  int w_, h_;
  Vector<uint32_t> pixel_data_;
  ZSpan zspan_;
  uint32_t value_to_store_;
  char *mask_;

  MPoly const *mpoly_;
  MLoop const *mloop_;
  MLoopUV const *mloopuv_;
  int totpoly_;
  int totloop_;
  int totedge_;

 public:
  TextureMarginMap(size_t w,
                   size_t h,
                   MPoly const *mpoly,
                   MLoop const *mloop,
                   MLoopUV const *mloopuv,
                   int totpoly,
                   int totloop,
                   int totedge)
      : w_(w),
        h_(h),
        mpoly_(mpoly),
        mloop_(mloop),
        mloopuv_(mloopuv),
        totpoly_(totpoly),
        totloop_(totloop),
        totedge_(totedge)
  {
    pixel_data_.resize(w_ * h_, 0xFFFFFFFF);

    zbuf_alloc_span(&zspan_, w_, h_);

    build_tables();
  }

  ~TextureMarginMap()
  {
    zbuf_free_span(&zspan_);
  }

  inline void set_pixel(int x, int y, uint32_t value)
  {
    BLI_assert(x < w_);
    BLI_assert(x >= 0);
    pixel_data_[y * w_ + x] = value;
  }

  inline uint32_t get_pixel(int x, int y) const
  {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) {
      return 0xFFFFFFFF;
    }

    return pixel_data_[y * w_ + x];
  }

  void rasterize_tri(float *v1, float *v2, float *v3, uint32_t value, char *mask)
  {
    /* NOTE: This is not thread safe, because the value to be written by the rasterizer is
     * a class member. If this is ever made multi-threaded each thread needs to get it's own. */
    value_to_store_ = value;
    mask_ = mask;
    zspan_scanconvert(
        &zspan_, this, &(v1[0]), &(v2[0]), &(v3[0]), TextureMarginMap::zscan_store_pixel);
  }

  static void zscan_store_pixel(
      void *map, int x, int y, [[maybe_unused]] float u, [[maybe_unused]] float v)
  {
    /* NOTE: Not thread safe, see comment above. */
    TextureMarginMap *m = static_cast<TextureMarginMap *>(map);
    m->set_pixel(x, y, m->value_to_store_);
    if (m->mask_) {
      m->mask_[y * m->w_ + x] = 1;
    }
  }

/* The map contains 2 kinds of pixels: DijkstraPixels and polygon indices. The top bit determines
 * what kind it is. With the top bit set, it is a 'dijkstra' pixel. The bottom 4 bits encode the
 * direction of the shortest path and the remaining 27 bits are used to store the distance. If
 * the top bit  is not set, the rest of the bits is used to store the polygon index.
 */
#define PackDijkstraPixel(dist, dir) (0x80000000 + ((dist) << 4) + (dir))
#define DijkstraPixelGetDistance(dp) (((dp) ^ 0x80000000) >> 4)
#define DijkstraPixelGetDirection(dp) ((dp)&0xF)
#define IsDijkstraPixel(dp) ((dp)&0x80000000)
#define DijkstraPixelIsUnset(dp) ((dp) == 0xFFFFFFFF)

  /**
   * Use dijkstra's algorithm to 'grow' a border around the polygons marked in the map.
   * For each pixel mark which direction is the shortest way to a polygon.
   */
  void grow_dijkstra(int margin)
  {
    class DijkstraActivePixel {
     public:
      DijkstraActivePixel(int dist, int _x, int _y) : distance(dist), x(_x), y(_y)
      {
      }
      int distance;
      int x, y;
    };
    auto cmp_dijkstrapixel_fun = [](DijkstraActivePixel const &a1, DijkstraActivePixel const &a2) {
      return a1.distance > a2.distance;
    };

    Vector<DijkstraActivePixel> active_pixels;
    for (int y = 0; y < h_; y++) {
      for (int x = 0; x < w_; x++) {
        if (DijkstraPixelIsUnset(get_pixel(x, y))) {
          for (int i = 0; i < 8; i++) {
            int xx = x - directions[i][0];
            int yy = y - directions[i][1];

            if (xx >= 0 && xx < w_ && yy >= 0 && yy < w_ && !IsDijkstraPixel(get_pixel(xx, yy))) {
              set_pixel(x, y, PackDijkstraPixel(distances[i], i));
              active_pixels.append(DijkstraActivePixel(distances[i], x, y));
              break;
            }
          }
        }
      }
    }

    /* Not strictly needed because at this point it already is a heap. */
#if 0
    std::make_heap(active_pixels.begin(), active_pixels.end(), cmp_dijkstrapixel_fun);
#endif

    while (active_pixels.size()) {
      std::pop_heap(active_pixels.begin(), active_pixels.end(), cmp_dijkstrapixel_fun);
      DijkstraActivePixel p = active_pixels.pop_last();

      int dist = p.distance;

      if (dist < 2 * (margin + 1)) {
        for (int i = 0; i < 8; i++) {
          int x = p.x + directions[i][0];
          int y = p.y + directions[i][1];
          if (x >= 0 && x < w_ && y >= 0 && y < h_) {
            uint32_t dp = get_pixel(x, y);
            if (IsDijkstraPixel(dp) && (DijkstraPixelGetDistance(dp) > dist + distances[i])) {
              BLI_assert(DijkstraPixelGetDirection(dp) != i);
              set_pixel(x, y, PackDijkstraPixel(dist + distances[i], i));
              active_pixels.append(DijkstraActivePixel(dist + distances[i], x, y));
              std::push_heap(active_pixels.begin(), active_pixels.end(), cmp_dijkstrapixel_fun);
            }
          }
        }
      }
    }
  }

  /**
   * Walk over the map and for margin pixels follow the direction stored in the bottom 3
   * bits back to the polygon.
   * Then look up the pixel from the next polygon.
   */
  void lookup_pixels(ImBuf *ibuf, char *mask, int maxPolygonSteps)
  {
    for (int y = 0; y < h_; y++) {
      for (int x = 0; x < w_; x++) {
        uint32_t dp = get_pixel(x, y);
        if (IsDijkstraPixel(dp) && !DijkstraPixelIsUnset(dp)) {
          int dist = DijkstraPixelGetDistance(dp);
          int direction = DijkstraPixelGetDirection(dp);

          int xx = x;
          int yy = y;

          /* Follow the dijkstra directions to find the polygon this margin pixels belongs to. */
          while (dist > 0) {
            xx -= directions[direction][0];
            yy -= directions[direction][1];
            dp = get_pixel(xx, yy);
            dist -= distances[direction];
            BLI_assert(!dist || (dist == DijkstraPixelGetDistance(dp)));
            direction = DijkstraPixelGetDirection(dp);
          }

          uint32_t poly = get_pixel(xx, yy);

          BLI_assert(!IsDijkstraPixel(poly));

          float destX, destY;

          int other_poly;
          bool found_pixel_in_polygon = false;
          if (lookup_pixel_polygon_neighbourhood(x, y, &poly, &destX, &destY, &other_poly)) {

            for (int i = 0; i < maxPolygonSteps; i++) {
              /* Force to pixel grid. */
              int nx = (int)round(destX);
              int ny = (int)round(destY);
              uint32_t polygon_from_map = get_pixel(nx, ny);
              if (other_poly == polygon_from_map) {
                found_pixel_in_polygon = true;
                break;
              }

              float dist_to_edge;
              /* Look up again, but starting from the polygon we were expected to land in. */
              if (!lookup_pixel(nx, ny, other_poly, &destX, &destY, &other_poly, &dist_to_edge)) {
                found_pixel_in_polygon = false;
                break;
              }
            }

            if (found_pixel_in_polygon) {
              bilinear_interpolation(ibuf, ibuf, destX, destY, x, y);
              /* Add our new pixels to the assigned pixel map. */
              mask[y * w_ + x] = 1;
            }
          }
        }
        else if (DijkstraPixelIsUnset(dp) || !IsDijkstraPixel(dp)) {
          /* These are not margin pixels, make sure the extend filter which is run after this step
           * leaves them alone.
           */
          mask[y * w_ + x] = 1;
        }
      }
    }
  }

 private:
  float2 uv_to_xy(MLoopUV const &mloopuv) const
  {
    float2 ret;
    ret.x = ((mloopuv.uv[0] * w_) - (0.5f + 0.001f));
    ret.y = ((mloopuv.uv[1] * h_) - (0.5f + 0.001f));
    return ret;
  }

  void build_tables()
  {
    loop_to_poly_map_.resize(totloop_);
    for (int i = 0; i < totpoly_; i++) {
      for (int j = 0; j < mpoly_[i].totloop; j++) {
        int l = j + mpoly_[i].loopstart;
        loop_to_poly_map_[l] = i;
      }
    }

    loop_adjacency_map_.resize(totloop_, -1);

    Vector<int> tmpmap;
    tmpmap.resize(totedge_, -1);

    for (size_t i = 0; i < totloop_; i++) {
      int edge = mloop_[i].e;
      if (tmpmap[edge] == -1) {
        loop_adjacency_map_[i] = -1;
        tmpmap[edge] = i;
      }
      else {
        BLI_assert(tmpmap[edge] >= 0);
        loop_adjacency_map_[i] = tmpmap[edge];
        loop_adjacency_map_[tmpmap[edge]] = i;
      }
    }
  }

  /**
   * Call lookup_pixel for the start_poly. If that fails, try the adjacent polygons as well.
   * Because the Dijkstra is not very exact in determining which polygon is the closest, the
   * polygon we need can be the one next to the one the Dijkstra map provides. To prevent missing
   * pixels also check the neighboring polygons.
   */
  bool lookup_pixel_polygon_neighbourhood(
      float x, float y, uint32_t *r_start_poly, float *r_destx, float *r_desty, int *r_other_poly)
  {
    float found_dist;
    if (lookup_pixel(x, y, *r_start_poly, r_destx, r_desty, r_other_poly, &found_dist)) {
      return true;
    }

    int loopstart = mpoly_[*r_start_poly].loopstart;
    int totloop = mpoly_[*r_start_poly].totloop;

    float destx, desty;
    int foundpoly;

    float mindist = -1.f;

    /* Loop over all adjacent polygons and determine which edge is closest.
     * This could be optimized by only inspecting neighbors which are on the edge of an island.
     * But it seems fast enough for now and that would add a lot of complexity. */
    for (int i = 0; i < totloop; i++) {
      int otherloop = loop_adjacency_map_[i + loopstart];

      if (otherloop < 0) {
        continue;
      }

      uint32_t poly = loop_to_poly_map_[otherloop];

      if (lookup_pixel(x, y, poly, &destx, &desty, &foundpoly, &found_dist)) {
        if (mindist < 0.f || found_dist < mindist) {
          mindist = found_dist;
          *r_other_poly = foundpoly;
          *r_destx = destx;
          *r_desty = desty;
          *r_start_poly = poly;
        }
      }
    }

    return mindist >= 0.f;
  }

  /**
   * Find which edge of the src_poly is closest to x,y. Look up it's adjacent UV-edge and polygon.
   * Then return the location of the equivalent pixel in the other polygon.
   * Returns true if a new pixel location was found, false if it wasn't, which can happen if the
   * margin pixel is on a corner, or the UV-edge doesn't have an adjacent polygon.
   */
  bool lookup_pixel(float x,
                    float y,
                    int src_poly,
                    float *r_destx,
                    float *r_desty,
                    int *r_other_poly,
                    float *r_dist_to_edge)
  {
    float2 point(x, y);

    *r_destx = *r_desty = 0;

    int found_edge = -1;
    float found_dist = -1;
    float found_t = 0;

    /* Find the closest edge on which the point x,y can be projected.
     */
    for (size_t i = 0; i < mpoly_[src_poly].totloop; i++) {
      int l1 = mpoly_[src_poly].loopstart + i;
      int l2 = l1 + 1;
      if (l2 >= mpoly_[src_poly].loopstart + mpoly_[src_poly].totloop) {
        l2 = mpoly_[src_poly].loopstart;
      }
      /* edge points */
      float2 edgepoint1 = uv_to_xy(mloopuv_[l1]);
      float2 edgepoint2 = uv_to_xy(mloopuv_[l2]);
      /* Vector AB is the vector from the first edge point to the second edge point.
       * Vector AP is the vector from the first edge point to our point under investigation. */
      float2 ab = edgepoint2 - edgepoint1;
      float2 ap = point - edgepoint1;

      /* Project ap onto ab. */
      float dotv = math::dot(ab, ap);

      float ablensq = math::length_squared(ab);

      float t = dotv / ablensq;

      if (t >= 0.0 && t <= 1.0) {

        /* Find the point on the edge closest to P */
        float2 reflect_point = edgepoint1 + (t * ab);
        /* This is the vector to P, so 90 degrees out from the edge. */
        float2 reflect_vec = reflect_point - point;

        float reflectLen = sqrt(reflect_vec[0] * reflect_vec[0] + reflect_vec[1] * reflect_vec[1]);
        float cross = ab[0] * reflect_vec[1] - ab[1] * reflect_vec[0];
        /* Only if P is on the outside of the edge, which means the cross product is positive,
         * we consider this edge.
         */
        bool valid = (cross > 0.0);

        if (valid && (found_dist < 0 || reflectLen < found_dist)) {
          /* Stother_ab the info of the closest edge so far. */
          found_dist = reflectLen;
          found_t = t;
          found_edge = i + mpoly_[src_poly].loopstart;
        }
      }
    }

    if (found_edge < 0) {
      return false;
    }

    *r_dist_to_edge = found_dist;

    /* Get the 'other' edge. I.E. the UV edge from the neighbor polygon. */
    int other_edge = loop_adjacency_map_[found_edge];

    if (other_edge < 0) {
      return false;
    }

    int dst_poly = loop_to_poly_map_[other_edge];

    if (r_other_poly) {
      *r_other_poly = dst_poly;
    }

    int other_edge2 = other_edge + 1;
    if (other_edge2 >= mpoly_[dst_poly].loopstart + mpoly_[dst_poly].totloop) {
      other_edge2 = mpoly_[dst_poly].loopstart;
    }

    float2 other_edgepoint1 = uv_to_xy(mloopuv_[other_edge]);
    float2 other_edgepoint2 = uv_to_xy(mloopuv_[other_edge2]);

    /* Calculate the vector from the order edges last point to it's first point. */
    float2 other_ab = other_edgepoint1 - other_edgepoint2;
    float2 other_reflect_point = other_edgepoint2 + (found_t * other_ab);
    float2 perpendicular_other_ab;
    perpendicular_other_ab.x = other_ab.y;
    perpendicular_other_ab.y = -other_ab.x;

    /* The new point is dound_dist distance from other_reflect_point at a 90 degree angle to
     * other_ab */
    float2 new_point = other_reflect_point + (found_dist / math::length(perpendicular_other_ab)) *
                                                 perpendicular_other_ab;

    *r_destx = new_point.x;
    *r_desty = new_point.y;

    return true;
  }
};  // class TextureMarginMap

const int TextureMarginMap::directions[8][2] = {
    {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}};
const int TextureMarginMap::distances[8] = {2, 3, 2, 3, 2, 3, 2, 3};

static void generate_margin(ImBuf *ibuf,
                            char *mask,
                            const int margin,
                            const Mesh *me,
                            DerivedMesh *dm,
                            char const *uv_layer)
{

  MPoly *mpoly;
  MLoop *mloop;
  MLoopUV const *mloopuv;
  int totpoly, totloop, totedge;

  int tottri;
  MLoopTri const *looptri;
  MLoopTri *looptri_mem = nullptr;

  if (me) {
    BLI_assert(dm == nullptr);
    totpoly = me->totpoly;
    totloop = me->totloop;
    totedge = me->totedge;
    mpoly = me->mpoly;
    mloop = me->mloop;

    if ((uv_layer == nullptr) || (uv_layer[0] == '\0')) {
      mloopuv = static_cast<MLoopUV const *>(CustomData_get_layer(&me->ldata, CD_MLOOPUV));
    }
    else {
      int uv_id = CustomData_get_named_layer(&me->ldata, CD_MLOOPUV, uv_layer);
      mloopuv = static_cast<MLoopUV const *>(
          CustomData_get_layer_n(&me->ldata, CD_MLOOPUV, uv_id));
    }

    tottri = poly_to_tri_count(me->totpoly, me->totloop);
    looptri_mem = static_cast<MLoopTri *>(MEM_mallocN(sizeof(*looptri) * tottri, __func__));
    BKE_mesh_recalc_looptri(
        me->mloop, me->mpoly, me->mvert, me->totloop, me->totpoly, looptri_mem);
    looptri = looptri_mem;
  }
  else {
    BLI_assert(dm != nullptr);
    BLI_assert(me == nullptr);
    totpoly = dm->getNumPolys(dm);
    totedge = dm->getNumEdges(dm);
    totloop = dm->getNumLoops(dm);
    mpoly = dm->getPolyArray(dm);
    mloop = dm->getLoopArray(dm);
    mloopuv = (MLoopUV const *)dm->getLoopDataArray(dm, CD_MLOOPUV);

    looptri = dm->getLoopTriArray(dm);
    tottri = dm->getNumLoopTri(dm);
  }

  TextureMarginMap map(ibuf->x, ibuf->y, mpoly, mloop, mloopuv, totpoly, totloop, totedge);

  bool draw_new_mask = false;
  /* Now the map contains 3 sorts of values: 0xFFFFFFFF for empty pixels, `0x80000000 + polyindex`
   * for margin pixels, just `polyindex` for poly pixels. */
  if (mask) {
    mask = (char *)MEM_dupallocN(mask);
  }
  else {
    mask = (char *)MEM_callocN(sizeof(char) * ibuf->x * ibuf->y, __func__);
    draw_new_mask = true;
  }

  for (int i = 0; i < tottri; i++) {
    const MLoopTri *lt = &looptri[i];
    float vec[3][2];

    for (int a = 0; a < 3; a++) {
      const float *uv = mloopuv[lt->tri[a]].uv;

      /* NOTE(campbell): workaround for pixel aligned UVs which are common and can screw up our
       * intersection tests where a pixel gets in between 2 faces or the middle of a quad,
       * camera aligned quads also have this problem but they are less common.
       * Add a small offset to the UVs, fixes bug T18685. */
      vec[a][0] = uv[0] * (float)ibuf->x - (0.5f + 0.001f);
      vec[a][1] = uv[1] * (float)ibuf->y - (0.5f + 0.002f);
    }

    /* NOTE: we need the top bit for the dijkstra distance map. */
    BLI_assert(lt->poly < 0x80000000);

    map.rasterize_tri(vec[0], vec[1], vec[2], lt->poly, draw_new_mask ? mask : nullptr);
  }

  char *tmpmask = (char *)MEM_dupallocN(mask);
  /* Extend (with averaging) by 2 pixels. Those will be overwritten, but it
   *  helps linear interpolations on the edges of polygons. */
  IMB_filter_extend(ibuf, tmpmask, 2);
  MEM_freeN(tmpmask);

  map.grow_dijkstra(margin);

  /* Looking further than 3 polygons away leads to so much cumulative rounding
   * that it isn't worth it. So hard-code it to 3. */
  map.lookup_pixels(ibuf, mask, 3);

  /* Use the extend filter to fill in the missing pixels at the corners, not strictly correct, but
   * the visual difference seems very minimal. This also catches pixels we missed because of very
   * narrow polygons.
   */
  IMB_filter_extend(ibuf, mask, margin);

  MEM_freeN(mask);

  if (looptri_mem) {
    MEM_freeN(looptri_mem);
  }
}

}  // namespace blender::render::texturemargin

void RE_generate_texturemargin_adjacentfaces(
    ImBuf *ibuf, char *mask, const int margin, const Mesh *me, char const *uv_layer)
{
  blender::render::texturemargin::generate_margin(ibuf, mask, margin, me, nullptr, uv_layer);
}

void RE_generate_texturemargin_adjacentfaces_dm(ImBuf *ibuf,
                                                char *mask,
                                                const int margin,
                                                DerivedMesh *dm)
{
  blender::render::texturemargin::generate_margin(ibuf, mask, margin, nullptr, dm, nullptr);
}
