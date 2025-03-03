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
 */

/** \file
 * \ingroup spview3d
 */

#include "BKE_context.h"

#include "WM_api.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "ED_screen.h"

#include "view3d_intern.h"
#include "view3d_navigate.h" /* own include */

/* -------------------------------------------------------------------- */
/** \name View Move (Pan) Operator
 * \{ */

/* NOTE: these defines are saved in keymap files, do not change values but just add new ones */

void viewmove_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {VIEW_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},

      {VIEWROT_MODAL_SWITCH_ZOOM, "SWITCH_TO_ZOOM", 0, "Switch to Zoom"},
      {VIEWROT_MODAL_SWITCH_ROTATE, "SWITCH_TO_ROTATE", 0, "Switch to Rotate"},

      {0, NULL, 0, NULL, NULL},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "View3D Move Modal");

  /* this function is called for each spacetype, only needs to add map once */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "View3D Move Modal", modal_items);

  /* items for modal map */
  WM_modalkeymap_add_item(keymap, MIDDLEMOUSE, KM_RELEASE, KM_ANY, 0, VIEW_MODAL_CONFIRM);
  WM_modalkeymap_add_item(keymap, EVT_ESCKEY, KM_PRESS, KM_ANY, 0, VIEW_MODAL_CONFIRM);

  /* disabled mode switching for now, can re-implement better, later on */
#if 0
  WM_modalkeymap_add_item(keymap, LEFTMOUSE, KM_PRESS, KM_ANY, 0, VIEWROT_MODAL_SWITCH_ZOOM);
  WM_modalkeymap_add_item(keymap, LEFTCTRLKEY, KM_PRESS, KM_ANY, 0, VIEWROT_MODAL_SWITCH_ZOOM);
  WM_modalkeymap_add_item(
      keymap, LEFTSHIFTKEY, KM_RELEASE, KM_ANY, 0, VIEWROT_MODAL_SWITCH_ROTATE);
#endif

  /* assign map to operators */
  WM_modalkeymap_assign(keymap, "VIEW3D_OT_move");
}

static int viewmove_modal(bContext *C, wmOperator *op, const wmEvent *event)
{

  ViewOpsData *vod = op->customdata;
  short event_code = VIEW_PASS;
  bool use_autokey = false;
  int ret = OPERATOR_RUNNING_MODAL;

  /* execute the events */
  if (event->type == MOUSEMOVE) {
    event_code = VIEW_APPLY;
  }
  else if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case VIEW_MODAL_CONFIRM:
        event_code = VIEW_CONFIRM;
        break;
      case VIEWROT_MODAL_SWITCH_ZOOM:
        WM_operator_name_call(C, "VIEW3D_OT_zoom", WM_OP_INVOKE_DEFAULT, NULL);
        event_code = VIEW_CONFIRM;
        break;
      case VIEWROT_MODAL_SWITCH_ROTATE:
        WM_operator_name_call(C, "VIEW3D_OT_rotate", WM_OP_INVOKE_DEFAULT, NULL);
        event_code = VIEW_CONFIRM;
        break;
    }
  }
  else if (event->type == vod->init.event_type && event->val == KM_RELEASE) {
    event_code = VIEW_CONFIRM;
  }

  if (event_code == VIEW_APPLY) {
    viewmove_apply(vod, event->xy[0], event->xy[1]);
    if (ED_screen_animation_playing(CTX_wm_manager(C))) {
      use_autokey = true;
    }
  }
  else if (event_code == VIEW_CONFIRM) {
    use_autokey = true;
    ret = OPERATOR_FINISHED;
  }

  if (use_autokey) {
    ED_view3d_camera_lock_autokey(vod->v3d, vod->rv3d, C, false, true);
  }

  if (ret & OPERATOR_FINISHED) {
    viewops_data_free(C, op);
  }

  return ret;
}

static int viewmove_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ViewOpsData *vod;

  const bool use_cursor_init = RNA_boolean_get(op->ptr, "use_cursor_init");

  /* makes op->customdata */
  viewops_data_alloc(C, op);
  vod = op->customdata;
  if (RV3D_LOCK_FLAGS(vod->rv3d) & RV3D_LOCK_LOCATION) {
    viewops_data_free(C, op);
    return OPERATOR_PASS_THROUGH;
  }

  viewops_data_create(C,
                      op,
                      event,
                      (viewops_flag_from_prefs() & ~VIEWOPS_FLAG_ORBIT_SELECT) |
                          (use_cursor_init ? VIEWOPS_FLAG_USE_MOUSE_INIT : 0));

  ED_view3d_smooth_view_force_finish(C, vod->v3d, vod->region);

  if (event->type == MOUSEPAN) {
    /* invert it, trackpad scroll follows same principle as 2d windows this way */
    viewmove_apply(
        vod, 2 * event->xy[0] - event->prev_xy[0], 2 * event->xy[1] - event->prev_xy[1]);

    viewops_data_free(C, op);

    return OPERATOR_FINISHED;
  }

  /* add temp handler */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static void viewmove_cancel(bContext *C, wmOperator *op)
{
  viewops_data_free(C, op);
}

void VIEW3D_OT_move(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "Pan View";
  ot->description = "Move the view";
  ot->idname = "VIEW3D_OT_move";

  /* api callbacks */
  ot->invoke = viewmove_invoke;
  ot->modal = viewmove_modal;
  ot->poll = ED_operator_region_view3d_active;
  ot->cancel = viewmove_cancel;

  /* flags */
  ot->flag = OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_XY;

  /* properties */
  view3d_operator_properties_common(ot, V3D_OP_PROP_USE_MOUSE_INIT);
}

/** \} */
