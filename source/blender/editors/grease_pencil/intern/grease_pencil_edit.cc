/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_stack.hh"
#include "BLT_translation.h"

#include "DNA_material_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves_utils.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.h"
#include "BKE_report.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "DEG_depsgraph.hh"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_screen.hh"

#include "GEO_smooth_curves.hh"
#include "GEO_subdivide_curves.hh"

#include "WM_api.hh"

#include "UI_resources.hh"

namespace blender::ed::greasepencil {

bool active_grease_pencil_poll(bContext *C)
{
  Object *object = CTX_data_active_object(C);
  if (object == nullptr || object->type != OB_GREASE_PENCIL) {
    return false;
  }
  return true;
}

bool editable_grease_pencil_poll(bContext *C)
{
  Object *object = CTX_data_active_object(C);
  if (object == nullptr || object->type != OB_GREASE_PENCIL) {
    return false;
  }
  if (!ED_operator_object_active_editable_ex(C, object)) {
    return false;
  }
  if ((object->mode & OB_MODE_EDIT) == 0) {
    return false;
  }
  return true;
}

bool editable_grease_pencil_point_selection_poll(bContext *C)
{
  if (!editable_grease_pencil_poll(C)) {
    return false;
  }

  /* Allowed: point and segment selection mode, not allowed: stroke selection mode. */
  ToolSettings *ts = CTX_data_tool_settings(C);
  return (ts->gpencil_selectmode_edit != GP_SELECTMODE_STROKE);
}

bool grease_pencil_painting_poll(bContext *C)
{
  if (!active_grease_pencil_poll(C)) {
    return false;
  }
  Object *object = CTX_data_active_object(C);
  if ((object->mode & OB_MODE_PAINT_GREASE_PENCIL) == 0) {
    return false;
  }
  ToolSettings *ts = CTX_data_tool_settings(C);
  if (!ts || !ts->gp_paint) {
    return false;
  }
  return true;
}

static void keymap_grease_pencil_editing(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(
      keyconf, "Grease Pencil Edit Mode", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = editable_grease_pencil_poll;
}

static void keymap_grease_pencil_painting(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(
      keyconf, "Grease Pencil Paint Mode", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = grease_pencil_painting_poll;
}

/* -------------------------------------------------------------------- */
/** \name Smooth Stroke Operator
 * \{ */

static int grease_pencil_stroke_smooth_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const int iterations = RNA_int_get(op->ptr, "iterations");
  const float influence = RNA_float_get(op->ptr, "factor");
  const bool keep_shape = RNA_boolean_get(op->ptr, "keep_shape");
  const bool smooth_ends = RNA_boolean_get(op->ptr, "smooth_ends");

  const bool smooth_position = RNA_boolean_get(op->ptr, "smooth_position");
  const bool smooth_radius = RNA_boolean_get(op->ptr, "smooth_radius");
  const bool smooth_opacity = RNA_boolean_get(op->ptr, "smooth_opacity");

  if (!(smooth_position || smooth_radius || smooth_opacity)) {
    /* There's nothing to be smoothed, return. */
    return OPERATOR_FINISHED;
  }

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (curves.points_num() == 0) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }

    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
    const OffsetIndices points_by_curve = curves.points_by_curve();
    const VArray<bool> cyclic = curves.cyclic();
    const VArray<bool> point_selection = *curves.attributes().lookup_or_default<bool>(
        ".selection", bke::AttrDomain::Point, true);

    if (smooth_position) {
      bke::GSpanAttributeWriter positions = attributes.lookup_for_write_span("position");
      geometry::smooth_curve_attribute(strokes,
                                       points_by_curve,
                                       point_selection,
                                       cyclic,
                                       iterations,
                                       influence,
                                       smooth_ends,
                                       keep_shape,
                                       positions.span);
      positions.finish();
      changed = true;
    }
    if (smooth_opacity && info.drawing.opacities().is_span()) {
      bke::GSpanAttributeWriter opacities = attributes.lookup_for_write_span("opacity");
      geometry::smooth_curve_attribute(strokes,
                                       points_by_curve,
                                       point_selection,
                                       cyclic,
                                       iterations,
                                       influence,
                                       smooth_ends,
                                       false,
                                       opacities.span);
      opacities.finish();
      changed = true;
    }
    if (smooth_radius && info.drawing.radii().is_span()) {
      bke::GSpanAttributeWriter radii = attributes.lookup_for_write_span("radius");
      geometry::smooth_curve_attribute(strokes,
                                       points_by_curve,
                                       point_selection,
                                       cyclic,
                                       iterations,
                                       influence,
                                       smooth_ends,
                                       false,
                                       radii.span);
      radii.finish();
      changed = true;
    }
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_smooth(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Smooth Stroke";
  ot->idname = "GREASE_PENCIL_OT_stroke_smooth";
  ot->description = "Smooth selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_smooth_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Smooth parameters. */
  prop = RNA_def_int(ot->srna, "iterations", 10, 1, 100, "Iterations", "", 1, 30);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_float(ot->srna, "factor", 1.0f, 0.0f, 1.0f, "Factor", "", 0.0f, 1.0f);
  RNA_def_boolean(ot->srna, "smooth_ends", false, "Smooth Endpoints", "");
  RNA_def_boolean(ot->srna, "keep_shape", false, "Keep Shape", "");

  RNA_def_boolean(ot->srna, "smooth_position", true, "Position", "");
  RNA_def_boolean(ot->srna, "smooth_radius", true, "Radius", "");
  RNA_def_boolean(ot->srna, "smooth_opacity", false, "Opacity", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simplify Stroke Operator
 * \{ */

static float dist_to_interpolated(
    float3 pos, float3 posA, float3 posB, float val, float valA, float valB)
{
  float dist1 = math::distance_squared(posA, pos);
  float dist2 = math::distance_squared(posB, pos);

  if (dist1 + dist2 > 0) {
    float interpolated_val = interpf(valB, valA, dist1 / (dist1 + dist2));
    return math::distance(interpolated_val, val);
  }
  return 0.0f;
}

static int64_t stroke_simplify(const IndexRange points,
                               const bool cyclic,
                               const float epsilon,
                               const FunctionRef<float(int64_t, int64_t, int64_t)> dist_function,
                               MutableSpan<bool> points_to_delete)
{
  int64_t total_points_to_delete = 0;
  const Span<bool> curve_selection = points_to_delete.slice(points);
  if (!curve_selection.contains(true)) {
    return total_points_to_delete;
  }

  const bool is_last_segment_selected = (curve_selection.first() && curve_selection.last());

  const Vector<IndexRange> selection_ranges = array_utils::find_all_ranges(curve_selection, true);
  threading::parallel_for(
      selection_ranges.index_range(), 1024, [&](const IndexRange range_of_ranges) {
        for (const IndexRange range : selection_ranges.as_span().slice(range_of_ranges)) {
          total_points_to_delete += ramer_douglas_peucker_simplify(
              range.shift(points.start()), epsilon, dist_function, points_to_delete);
        }
      });

  /* For cyclic curves, simplify the last segment. */
  if (cyclic && points.size() > 2 && is_last_segment_selected) {
    const float dist = dist_function(points.last(1), points.first(), points.last());
    if (dist <= epsilon) {
      points_to_delete[points.last()] = true;
      total_points_to_delete++;
    }
  }

  return total_points_to_delete;
}

static int grease_pencil_stroke_simplify_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const float epsilon = RNA_float_get(op->ptr, "factor");

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (curves.points_num() == 0) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }

    const Span<float3> positions = curves.positions();
    const VArray<float> radii = info.drawing.radii();

    /* Distance functions for `ramer_douglas_peucker_simplify`. */
    const auto dist_function_positions =
        [positions](int64_t first_index, int64_t last_index, int64_t index) {
          const float dist_position = dist_to_line_v3(
              positions[index], positions[first_index], positions[last_index]);
          return dist_position;
        };
    const auto dist_function_positions_and_radii =
        [positions, radii](int64_t first_index, int64_t last_index, int64_t index) {
          const float dist_position = dist_to_line_v3(
              positions[index], positions[first_index], positions[last_index]);
          /* We divide the distance by 2000.0f to convert from "pixels" to an actual
           * distance. For some reason, grease pencil strokes the thickness of strokes in
           * pixels rather than object space distance. */
          const float dist_radii = dist_to_interpolated(positions[index],
                                                        positions[first_index],
                                                        positions[last_index],
                                                        radii[index],
                                                        radii[first_index],
                                                        radii[last_index]) /
                                   2000.0f;
          return math::max(dist_position, dist_radii);
        };

    const VArray<bool> cyclic = curves.cyclic();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
        ".selection", bke::AttrDomain::Point, true);

    /* Mark all points in the editable curves to be deleted. */
    Array<bool> points_to_delete(curves.points_num(), false);
    bke::curves::fill_points(points_by_curve, strokes, true, points_to_delete.as_mutable_span());

    std::atomic<int64_t> total_points_to_delete = 0;
    if (radii.is_single()) {
      strokes.foreach_index([&](const int64_t curve_i) {
        const IndexRange points = points_by_curve[curve_i];
        total_points_to_delete += stroke_simplify(points,
                                                  cyclic[curve_i],
                                                  epsilon,
                                                  dist_function_positions,
                                                  points_to_delete.as_mutable_span());
      });
    }
    else if (radii.is_span()) {
      strokes.foreach_index([&](const int64_t curve_i) {
        const IndexRange points = points_by_curve[curve_i];
        total_points_to_delete += stroke_simplify(points,
                                                  cyclic[curve_i],
                                                  epsilon,
                                                  dist_function_positions_and_radii,
                                                  points_to_delete.as_mutable_span());
      });
    }

    if (total_points_to_delete > 0) {
      IndexMaskMemory memory;
      curves.remove_points(IndexMask::from_bools(points_to_delete, memory), {});
      info.drawing.tag_topology_changed();
      changed = true;
    }
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_simplify(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Simplify Stroke";
  ot->idname = "GREASE_PENCIL_OT_stroke_simplify";
  ot->description = "Simplify selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_simplify_exec;
  ot->poll = editable_grease_pencil_point_selection_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  prop = RNA_def_float(ot->srna, "factor", 0.01f, 0.0f, 100.0f, "Factor", "", 0.0f, 100.0f);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Operator
 * \{ */

static bke::CurvesGeometry remove_points_and_split(const bke::CurvesGeometry &curves,
                                                   const IndexMask &mask)
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const VArray<bool> src_cyclic = curves.cyclic();

  Array<bool> points_to_delete(curves.points_num());
  mask.to_bools(points_to_delete.as_mutable_span());
  const int total_points = points_to_delete.as_span().count(false);

  /* Return if deleting everything. */
  if (total_points == 0) {
    return {};
  }

  int curr_dst_point_id = 0;
  Array<int> dst_to_src_point(total_points);
  Vector<int> dst_curve_counts;
  Vector<int> dst_to_src_curve;
  Vector<bool> dst_cyclic;

  for (const int curve_i : curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const Span<bool> curve_points_to_delete = points_to_delete.as_span().slice(points);
    const bool curve_cyclic = src_cyclic[curve_i];

    /* Note, these ranges start at zero and needed to be shifted by `points.first()` */
    const Vector<IndexRange> ranges_to_keep = array_utils::find_all_ranges(curve_points_to_delete,
                                                                           false);

    if (ranges_to_keep.is_empty()) {
      continue;
    }

    const bool is_last_segment_selected = curve_cyclic && ranges_to_keep.first().first() == 0 &&
                                          ranges_to_keep.last().last() == points.size() - 1;
    const bool is_curve_self_joined = is_last_segment_selected && ranges_to_keep.size() != 1;
    const bool is_cyclic = ranges_to_keep.size() == 1 && is_last_segment_selected;

    IndexRange range_ids = ranges_to_keep.index_range();
    /* Skip the first range because it is joined to the end of the last range. */
    for (const int range_i : ranges_to_keep.index_range().drop_front(is_curve_self_joined)) {
      const IndexRange range = ranges_to_keep[range_i];

      int count = range.size();
      for (const int src_point : range.shift(points.first())) {
        dst_to_src_point[curr_dst_point_id++] = src_point;
      }

      /* Join the first range to the end of the last range. */
      if (is_curve_self_joined && range_i == range_ids.last()) {
        const IndexRange first_range = ranges_to_keep[range_ids.first()];
        for (const int src_point : first_range.shift(points.first())) {
          dst_to_src_point[curr_dst_point_id++] = src_point;
        }
        count += first_range.size();
      }

      dst_curve_counts.append(count);
      dst_to_src_curve.append(curve_i);
      dst_cyclic.append(is_cyclic);
    }
  }

  const int total_curves = dst_to_src_curve.size();

  bke::CurvesGeometry dst_curves(total_points, total_curves);

  MutableSpan<int> new_curve_offsets = dst_curves.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(), new_curve_offsets.drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets);

  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
  const bke::AttributeAccessor src_attributes = curves.attributes();

  /* Transfer curve attributes. */
  gather_attributes(
      src_attributes, bke::AttrDomain::Curve, {}, {"cyclic"}, dst_to_src_curve, dst_attributes);
  array_utils::copy(dst_cyclic.as_span(), dst_curves.cyclic_for_write());

  /* Transfer point attributes. */
  gather_attributes(
      src_attributes, bke::AttrDomain::Point, {}, {}, dst_to_src_point, dst_attributes);

  dst_curves.update_curve_types();
  dst_curves.remove_attributes_based_on_types();

  return dst_curves;
}

static int grease_pencil_delete_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask elements = ed::greasepencil::retrieve_editable_and_selected_elements(
        *object, info.drawing, selection_domain, memory);
    if (elements.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (selection_domain == bke::AttrDomain::Curve) {
      curves.remove_curves(elements, {});
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      curves = remove_points_and_split(curves, elements);
    }
    info.drawing.tag_topology_changed();
    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_delete(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Delete";
  ot->idname = "GREASE_PENCIL_OT_delete";
  ot->description = "Delete selected strokes or points";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_delete_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dissolve Points Operator
 * \{ */

enum class DissolveMode : int8_t {
  /** Dissolve all selected points. */
  POINTS = 0,
  /** Dissolve between selected points. */
  BETWEEN = 1,
  /** Dissolve unselected points. */
  UNSELECT = 2,
};

static const EnumPropertyItem prop_dissolve_types[] = {
    {int(DissolveMode::POINTS), "POINTS", 0, "Dissolve", "Dissolve selected points"},
    {int(DissolveMode::BETWEEN),
     "BETWEEN",
     0,
     "Dissolve Between",
     "Dissolve points between selected points"},
    {int(DissolveMode::UNSELECT),
     "UNSELECT",
     0,
     "Dissolve Unselect",
     "Dissolve all unselected points"},
    {0, nullptr, 0, nullptr, nullptr},
};

static Array<bool> get_points_to_dissolve(bke::CurvesGeometry &curves,
                                          const IndexMask &mask,
                                          const DissolveMode mode)
{
  const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
      ".selection", bke::AttrDomain::Point, true);

  Array<bool> points_to_dissolve(curves.points_num(), false);
  selection.materialize(mask, points_to_dissolve);

  if (mode == DissolveMode::POINTS) {
    return points_to_dissolve;
  }

  /* Both `between` and `unselect` have the unselected point being the ones dissolved so we need
   * to invert. */
  BLI_assert(ELEM(mode, DissolveMode::BETWEEN, DissolveMode::UNSELECT));

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  /* Because we are going to invert, these become the points to keep. */
  MutableSpan<bool> points_to_keep = points_to_dissolve.as_mutable_span();

  threading::parallel_for(curves.curves_range(), 128, [&](const IndexRange range) {
    for (const int64_t curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];
      const Span<bool> curve_selection = points_to_dissolve.as_span().slice(points);
      /* The unselected curves should not be dissolved. */
      if (!curve_selection.contains(true)) {
        points_to_keep.slice(points).fill(true);
        continue;
      }

      /* `between` is just `unselect` but with the first and last segments not getting
       * dissolved. */
      if (mode != DissolveMode::BETWEEN) {
        continue;
      }

      const Vector<IndexRange> deselection_ranges = array_utils::find_all_ranges(curve_selection,
                                                                                 false);

      if (deselection_ranges.size() != 0) {
        const IndexRange first_range = deselection_ranges.first().shift(points.first());
        const IndexRange last_range = deselection_ranges.last().shift(points.first());

        /* Ranges should only be fill if the first/last point matches the start/end point
         * of the segment. */
        if (first_range.first() == points.first()) {
          points_to_keep.slice(first_range).fill(true);
        }
        if (last_range.last() == points.last()) {
          points_to_keep.slice(last_range).fill(true);
        }
      }
    }
  });

  array_utils::invert_booleans(points_to_dissolve);

  return points_to_dissolve;
}

static int grease_pencil_dissolve_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const DissolveMode mode = DissolveMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (curves.points_num() == 0) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask points = ed::greasepencil::retrieve_editable_and_selected_points(
        *object, info.drawing, memory);
    if (points.is_empty()) {
      return;
    }

    const Array<bool> points_to_dissolve = get_points_to_dissolve(curves, points, mode);
    if (points_to_dissolve.as_span().contains(true)) {
      curves.remove_points(IndexMask::from_bools(points_to_dissolve, memory), {});
      info.drawing.tag_topology_changed();
      changed = true;
    }
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_dissolve(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Dissolve";
  ot->idname = "GREASE_PENCIL_OT_dissolve";
  ot->description = "Delete selected points without splitting strokes";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_dissolve_exec;
  ot->poll = editable_grease_pencil_point_selection_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = prop = RNA_def_enum(ot->srna,
                                 "type",
                                 prop_dissolve_types,
                                 0,
                                 "Type",
                                 "Method used for dissolving stroke points");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Frame Operator
 * \{ */

enum class DeleteFrameMode : int8_t {
  /** Delete the active frame for the current layer. */
  ACTIVE_FRAME = 0,
  /** Delete the active frames for all layers. */
  ALL_FRAMES = 1,
};

static const EnumPropertyItem prop_greasepencil_deleteframe_types[] = {
    {int(DeleteFrameMode::ACTIVE_FRAME),
     "ACTIVE_FRAME",
     0,
     "Active Frame",
     "Deletes current frame in the active layer"},
    {int(DeleteFrameMode::ALL_FRAMES),
     "ALL_FRAMES",
     0,
     "All Active Frames",
     "Delete active frames for all layers"},
    {0, nullptr, 0, nullptr, nullptr},
};

static int grease_pencil_delete_frame_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const int current_frame = scene->r.cfra;

  const DeleteFrameMode mode = DeleteFrameMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  if (mode == DeleteFrameMode::ACTIVE_FRAME && grease_pencil.has_active_layer()) {
    bke::greasepencil::Layer &layer = *grease_pencil.get_active_layer();
    if (layer.is_editable() && layer.frame_key_at(current_frame)) {
      changed |= grease_pencil.remove_frames(layer, {*layer.frame_key_at(current_frame)});
    }
  }
  else if (mode == DeleteFrameMode::ALL_FRAMES) {
    for (bke::greasepencil::Layer *layer : grease_pencil.layers_for_write()) {
      if (layer->is_editable() && layer->frame_key_at(current_frame)) {
        changed |= grease_pencil.remove_frames(*layer, {*layer->frame_key_at(current_frame)});
      }
    }
  }

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);
    WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_delete_frame(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Delete Frame";
  ot->idname = "GREASE_PENCIL_OT_delete_frame";
  ot->description = "Delete Grease Pencil Frame(s)";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_delete_frame_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = prop = RNA_def_enum(ot->srna,
                                 "type",
                                 prop_greasepencil_deleteframe_types,
                                 0,
                                 "Type",
                                 "Method used for deleting Grease Pencil frames");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Material Set Operator
 * \{ */

static int grease_pencil_stroke_material_set_exec(bContext *C, wmOperator *op)
{
  using namespace blender;
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  Material *ma = nullptr;
  char name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "material", name);

  int material_index = object->actcol - 1;

  if (name[0] != '\0') {
    ma = reinterpret_cast<Material *>(BKE_libblock_find_name(bmain, ID_MA, name));
    if (ma == nullptr) {
      BKE_reportf(op->reports, RPT_WARNING, TIP_("Material '%s' could not be found"), name);
      return OPERATOR_CANCELLED;
    }

    /* Find slot index. */
    material_index = BKE_object_material_index_get(object, ma);
  }

  if (material_index == -1) {
    return OPERATOR_CANCELLED;
  }

  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    bke::SpanAttributeWriter<int> materials =
        curves.attributes_for_write().lookup_or_add_for_write_span<int>("material_index",
                                                                        bke::AttrDomain::Curve);
    index_mask::masked_fill(materials.span, material_index, strokes);
    materials.finish();
  });

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_material_set(wmOperatorType *ot)
{
  ot->name = "Assign Material";
  ot->idname = "GREASE_PENCIL_OT_stroke_material_set";
  ot->description = "Assign the active material slot to the selected strokes";

  ot->exec = grease_pencil_stroke_material_set_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_string(
      ot->srna, "material", nullptr, MAX_ID_NAME - 2, "Material", "Name of the material");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Cyclical Set Operator
 * \{ */

enum class CyclicalMode : int8_t {
  /** Sets all strokes to cycle. */
  CLOSE = 0,
  /** Sets all strokes to not cycle. */
  OPEN = 1,
  /** Switches the cyclic state of the strokes. */
  TOGGLE = 2,
};

static const EnumPropertyItem prop_cyclical_types[] = {
    {int(CyclicalMode::CLOSE), "CLOSE", 0, "Close All", ""},
    {int(CyclicalMode::OPEN), "OPEN", 0, "Open All", ""},
    {int(CyclicalMode::TOGGLE), "TOGGLE", 0, "Toggle", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static int grease_pencil_cyclical_set_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const CyclicalMode mode = CyclicalMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (mode == CyclicalMode::OPEN && !curves.attributes().contains("cyclic")) {
      /* Avoid creating unneeded attribute. */
      return;
    }

    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }

    MutableSpan<bool> cyclic = curves.cyclic_for_write();
    switch (mode) {
      case CyclicalMode::CLOSE:
        index_mask::masked_fill(cyclic, true, strokes);
        break;
      case CyclicalMode::OPEN:
        index_mask::masked_fill(cyclic, false, strokes);
        break;
      case CyclicalMode::TOGGLE:
        array_utils::invert_booleans(cyclic, strokes);
        break;
    }

    /* Remove the attribute if it is empty. */
    if (mode != CyclicalMode::CLOSE) {
      if (array_utils::booleans_mix_calc(curves.cyclic()) == array_utils::BooleanMix::AllFalse) {
        curves.attributes_for_write().remove("cyclic");
      }
    }

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_cyclical_set(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Cyclical State";
  ot->idname = "GREASE_PENCIL_OT_cyclical_set";
  ot->description = "Close or open the selected stroke adding a segment from last to first point";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_cyclical_set_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = RNA_def_enum(
      ot->srna, "type", prop_cyclical_types, int(CyclicalMode::TOGGLE), "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Active Material Operator
 * \{ */

static int grease_pencil_set_active_material_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  if (object->totcol == 0) {
    return OPERATOR_CANCELLED;
  }

  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  for (const MutableDrawingInfo &info : drawings) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      continue;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const VArray<int> materials = *curves.attributes().lookup_or_default<int>(
        "material_index", bke::AttrDomain::Curve, 0);
    object->actcol = materials[strokes.first()] + 1;
    break;
  };

  WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_active_material(wmOperatorType *ot)
{
  ot->name = "Set Active Material";
  ot->idname = "GREASE_PENCIL_OT_set_active_material";
  ot->description = "Set the selected stroke material as the active material";

  ot->exec = grease_pencil_set_active_material_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Uniform Thickness Operator
 * \{ */

static int grease_pencil_set_uniform_thickness_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  /* Radius is half of the thickness. */
  const float radius = RNA_float_get(op->ptr, "thickness") * 0.5f;

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    MutableSpan<float> radii = info.drawing.radii_for_write();
    bke::curves::fill_points<float>(points_by_curve, strokes, radius, radii);
    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_uniform_thickness(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Uniform Thickness";
  ot->idname = "GREASE_PENCIL_OT_set_uniform_thickness";
  ot->description = "Set all stroke points to same thickness";

  /* Callbacks. */
  ot->exec = grease_pencil_set_uniform_thickness_exec;
  ot->poll = editable_grease_pencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ot->prop = RNA_def_float(
      ot->srna, "thickness", 0.1f, 0.0f, 1000.0f, "Thickness", "Thickness", 0.0f, 1000.0f);
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Set Uniform Opacity Operator
 * \{ */

static int grease_pencil_set_uniform_opacity_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const float opacity = RNA_float_get(op->ptr, "opacity");

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    MutableSpan<float> opacities = info.drawing.opacities_for_write();
    bke::curves::fill_points<float>(points_by_curve, strokes, opacity, opacities);
    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_uniform_opacity(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Uniform Opacity";
  ot->idname = "GREASE_PENCIL_OT_set_uniform_opacity";
  ot->description = "Set all stroke points to same opacity";

  /* Callbacks. */
  ot->exec = grease_pencil_set_uniform_opacity_exec;
  ot->poll = editable_grease_pencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ot->prop = RNA_def_float(ot->srna, "opacity", 1.0f, 0.0f, 1.0f, "Opacity", "", 0.0f, 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Switch Direction Operator
 * \{ */

static int grease_pencil_stroke_switch_direction_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    /* Switch stroke direction. */
    curves.reverse_curves(strokes);

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_switch_direction(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Switch Direction";
  ot->idname = "GREASE_PENCIL_OT_stroke_switch_direction";
  ot->description = "Change direction of the points of the selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_switch_direction_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Curve Caps Operator
 * \{ */

enum class CapsMode : int8_t {
  /** Switches both to Flat. */
  FLAT = 0,
  /** Change only start. */
  START = 1,
  /** Change only end. */
  END = 2,
  /** Switches both to default rounded. */
  ROUND = 3,
};

static void toggle_caps(MutableSpan<int8_t> caps, const IndexMask &strokes)
{
  strokes.foreach_index([&](const int stroke_i) {
    if (caps[stroke_i] == GP_STROKE_CAP_FLAT) {
      caps[stroke_i] = GP_STROKE_CAP_ROUND;
    }
    else {
      caps[stroke_i] = GP_STROKE_CAP_FLAT;
    }
  });
}

static int grease_pencil_caps_set_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const CapsMode mode = CapsMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }

    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

    if (ELEM(mode, CapsMode::ROUND, CapsMode::FLAT)) {
      bke::SpanAttributeWriter<int8_t> start_caps =
          attributes.lookup_or_add_for_write_span<int8_t>("start_cap", bke::AttrDomain::Curve);
      bke::SpanAttributeWriter<int8_t> end_caps = attributes.lookup_or_add_for_write_span<int8_t>(
          "end_cap", bke::AttrDomain::Curve);

      const int8_t flag_set = (mode == CapsMode::ROUND) ? int8_t(GP_STROKE_CAP_TYPE_ROUND) :
                                                          int8_t(GP_STROKE_CAP_TYPE_FLAT);

      index_mask::masked_fill(start_caps.span, flag_set, strokes);
      index_mask::masked_fill(end_caps.span, flag_set, strokes);
      start_caps.finish();
      end_caps.finish();
    }
    else {
      switch (mode) {
        case CapsMode::START: {
          bke::SpanAttributeWriter<int8_t> caps = attributes.lookup_or_add_for_write_span<int8_t>(
              "start_cap", bke::AttrDomain::Curve);
          toggle_caps(caps.span, strokes);
          caps.finish();
          break;
        }
        case CapsMode::END: {
          bke::SpanAttributeWriter<int8_t> caps = attributes.lookup_or_add_for_write_span<int8_t>(
              "end_cap", bke::AttrDomain::Curve);
          toggle_caps(caps.span, strokes);
          caps.finish();
          break;
        }
        case CapsMode::ROUND:
        case CapsMode::FLAT:
          break;
      }
    }

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_caps_set(wmOperatorType *ot)
{
  static const EnumPropertyItem prop_caps_types[] = {
      {int(CapsMode::ROUND), "ROUND", 0, "Rounded", "Set as default rounded"},
      {int(CapsMode::FLAT), "FLAT", 0, "Flat", ""},
      RNA_ENUM_ITEM_SEPR,
      {int(CapsMode::START), "START", 0, "Toggle Start", ""},
      {int(CapsMode::END), "END", 0, "Toggle End", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* Identifiers. */
  ot->name = "Set Curve Caps";
  ot->idname = "GREASE_PENCIL_OT_caps_set";
  ot->description = "Change curve caps mode (rounded or flat)";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_caps_set_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = RNA_def_enum(ot->srna, "type", prop_caps_types, int(CapsMode::ROUND), "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Active Material Operator
 * \{ */

/* Retry enum items with object materials. */
static const EnumPropertyItem *material_enum_itemf(bContext *C,
                                                   PointerRNA * /*ptr*/,
                                                   PropertyRNA * /*prop*/,
                                                   bool *r_free)
{
  Object *ob = CTX_data_active_object(C);
  EnumPropertyItem *item = nullptr, item_tmp = {0};
  int totitem = 0;

  if (ob == nullptr) {
    return rna_enum_dummy_DEFAULT_items;
  }

  /* Existing materials */
  for (const int i : IndexRange(ob->totcol)) {
    if (Material *ma = BKE_object_material_get(ob, i + 1)) {
      item_tmp.identifier = ma->id.name + 2;
      item_tmp.name = ma->id.name + 2;
      item_tmp.value = i + 1;
      item_tmp.icon = ma->preview ? ma->preview->icon_id : ICON_NONE;

      RNA_enum_item_add(&item, &totitem, &item_tmp);
    }
  }
  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static int grease_pencil_set_material_exec(bContext *C, wmOperator *op)
{
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const int slot = RNA_enum_get(op->ptr, "slot");

  /* Try to get material slot. */
  if ((slot < 1) || (slot > object->totcol)) {
    return OPERATOR_CANCELLED;
  }

  /* Set active material. */
  object->actcol = slot;

  WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_material(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Active Material";
  ot->idname = "GREASE_PENCIL_OT_set_material";
  ot->description = "Set active material";

  /* callbacks */
  ot->exec = grease_pencil_set_material_exec;
  ot->poll = active_grease_pencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Material to use (dynamic enum) */
  ot->prop = RNA_def_enum(ot->srna, "slot", rna_enum_dummy_DEFAULT_items, 0, "Material Slot", "");
  RNA_def_enum_funcs(ot->prop, material_enum_itemf);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Operator
 * \{ */

static int grease_pencil_duplicate_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  std::atomic<bool> changed = false;
  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask elements = retrieve_editable_and_selected_elements(
        *object, info.drawing, selection_domain, memory);
    if (elements.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (selection_domain == bke::AttrDomain::Curve) {
      curves::duplicate_curves(curves, elements);
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      curves::duplicate_points(curves, elements);
    }
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_duplicate(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Duplicate";
  ot->idname = "GREASE_PENCIL_OT_duplicate";
  ot->description = "Duplicate the selected points";

  /* Callbacks. */
  ot->exec = grease_pencil_duplicate_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int grease_pencil_clean_loose_exec(bContext *C, wmOperator *op)
{
  Object *object = CTX_data_active_object(C);
  Scene &scene = *CTX_data_scene(C);
  const int limit = RNA_int_get(op->ptr, "limit");

  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);

  threading::parallel_for_each(drawings, [&](MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();

    IndexMaskMemory memory;
    const IndexMask editable_strokes = ed::greasepencil::retrieve_editable_strokes(
        *object, info.drawing, memory);

    const IndexMask curves_to_delete = IndexMask::from_predicate(
        editable_strokes, GrainSize(4096), memory, [&](const int i) {
          return points_by_curve[i].size() <= limit;
        });

    curves.remove_curves(curves_to_delete, {});
  });

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_clean_loose(wmOperatorType *ot)
{
  ot->name = "Clean Loose Points";
  ot->idname = "GREASE_PENCIL_OT_clean_loose";
  ot->description = "Remove loose points";

  ot->invoke = WM_operator_props_popup_confirm;
  ot->exec = grease_pencil_clean_loose_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "limit",
              1,
              1,
              INT_MAX,
              "Limit",
              "Number of points to consider stroke as loose",
              1,
              INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Subdivide Operator
 * \{ */

static int gpencil_stroke_subdivide_exec(bContext *C, wmOperator *op)
{
  const int cuts = RNA_int_get(op->ptr, "number_cuts");
  const bool only_selected = RNA_boolean_get(op->ptr, "only_selected");

  std::atomic<bool> changed = false;

  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  const Array<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);

  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    VArray<int> vcuts = {};

    if (selection_domain == bke::AttrDomain::Curve || !only_selected) {
      /* Subdivide entire selected curve, every stroke subdivides to the same cut. */
      vcuts = VArray<int>::ForSingle(cuts, curves.points_num());
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      /* Subdivide between selected points. Only cut between selected points.
       * Make the cut array the same length as point count for specifying
       * cut/uncut for each segment. */
      const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
          ".selection", bke::AttrDomain::Point, true);

      const OffsetIndices points_by_curve = curves.points_by_curve();
      const VArray<bool> cyclic = curves.cyclic();

      Array<int> use_cuts(curves.points_num(), 0);

      /* The cut is after each point, so the last point selected wouldn't need to be registered. */
      for (const int curve : curves.curves_range()) {
        /* No need to loop to the last point since the cut is registered on the point before the
         * segment. */
        for (const int point : points_by_curve[curve].drop_back(1)) {
          /* The point itself should be selected. */
          if (!selection[point]) {
            continue;
          }
          /* If the next point in the curve is selected, then cut this segment. */
          if (selection[point + 1]) {
            use_cuts[point] = cuts;
          }
        }
        /* Check for cyclic and selection. */
        if (cyclic[curve]) {
          const int first_point = points_by_curve[curve].first();
          const int last_point = points_by_curve[curve].last();
          if (selection[first_point] && selection[last_point]) {
            use_cuts[last_point] = cuts;
          }
        }
      }
      vcuts = VArray<int>::ForContainer(std::move(use_cuts));
    }

    curves = geometry::subdivide_curves(curves, strokes, vcuts, {});
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_subdivide(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Subdivide Stroke";
  ot->idname = "GREASE_PENCIL_OT_stroke_subdivide";
  ot->description =
      "Subdivide between continuous selected points of the stroke adding a point half way "
      "between "
      "them";

  /* API callbacks. */
  ot->exec = gpencil_stroke_subdivide_exec;
  ot->poll = ed::greasepencil::editable_grease_pencil_poll;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Properties. */
  prop = RNA_def_int(ot->srna, "number_cuts", 1, 1, 32, "Number of Cuts", "", 1, 5);
  /* Avoid re-using last var because it can cause _very_ high value and annoy users. */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  RNA_def_boolean(ot->srna,
                  "only_selected",
                  true,
                  "Selected Points",
                  "Smooth only selected points in the stroke");
}

/** \} */

static void grease_pencil_operatormarcos_define()
{
  wmOperatorType *ot;

  ot = WM_operatortype_append_macro("GREASE_PENCIL_OT_stroke_subdivide_smooth",
                                    "Subdivide and Smooth",
                                    "Subdivide strokes and smooth them",
                                    OPTYPE_UNDO | OPTYPE_REGISTER);
  WM_operatortype_macro_define(ot, "GREASE_PENCIL_OT_stroke_subdivide");
  WM_operatortype_macro_define(ot, "GREASE_PENCIL_OT_stroke_smooth");
}

}  // namespace blender::ed::greasepencil

void ED_operatortypes_grease_pencil_edit()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_smooth);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_simplify);
  WM_operatortype_append(GREASE_PENCIL_OT_delete);
  WM_operatortype_append(GREASE_PENCIL_OT_dissolve);
  WM_operatortype_append(GREASE_PENCIL_OT_delete_frame);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_material_set);
  WM_operatortype_append(GREASE_PENCIL_OT_cyclical_set);
  WM_operatortype_append(GREASE_PENCIL_OT_set_active_material);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_switch_direction);
  WM_operatortype_append(GREASE_PENCIL_OT_set_uniform_thickness);
  WM_operatortype_append(GREASE_PENCIL_OT_set_uniform_opacity);
  WM_operatortype_append(GREASE_PENCIL_OT_caps_set);
  WM_operatortype_append(GREASE_PENCIL_OT_duplicate);
  WM_operatortype_append(GREASE_PENCIL_OT_set_material);
  WM_operatortype_append(GREASE_PENCIL_OT_clean_loose);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_subdivide);

  grease_pencil_operatormarcos_define();
}

void ED_keymap_grease_pencil(wmKeyConfig *keyconf)
{
  using namespace blender::ed::greasepencil;
  keymap_grease_pencil_editing(keyconf);
  keymap_grease_pencil_painting(keyconf);
}
