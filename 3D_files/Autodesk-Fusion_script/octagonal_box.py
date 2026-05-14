"""
Parametric Octagonal Storage Box with Twist-Lock Lid
Autodesk Fusion 360 Script

Twist-lock (bayonet) design:
  Box inner wall: N L-shaped slots (vertical entry + horizontal lock channel).
  Lid: cylindrical plug with N outward tabs.
  To close: align tabs with entry slots, press lid down, rotate CCW LOCK_ROT_DEG.
  To open: rotate CW, pull up.

Change values in Parameters section only. All dims mm.
"""

import adsk.core
import adsk.fusion
import adsk.cam
import traceback
import math

# ── Parameters ────────────────────────────────────────────────────────────────
CYLINDER_DIAM   = 90.0    # inner cavity diameter
CYLINDER_HEIGHT = 80.0    # inner cavity height
WALL            = 6.6     # wall thickness — keep ≥ TAB_DEPTH + TAB_CLEAR_R + 1.0
FLOOR           = 3.0     # floor thickness

HOLE_SMALL_DIAM = 10.75   # side hole (+X face): diameter
HOLE_SMALL_BOT  = -5.0    # side hole (+X face): bottom edge Z from box bottom
HOLE_SMALL_Y    = -20.0     # side hole (+X face): lateral offset from face centre
HOLE_LARGE_DIAM = 15.0    # side hole (-X face): diameter
HOLE_LARGE_BOT  = -5.0    # side hole (-X face): bottom edge Z from box bottom
HOLE_LARGE_Y    = -20.0     # side hole (-X face): lateral offset from face centre

LID_SLOT_W      = 17.0    # lid rectangle slot width
LID_SLOT_H      = 6.85    # lid rectangle slot height
LID_THICKNESS   = 10.0     # lid disc thickness
LID_GAP         = 25.0    # model-space gap lid above box

# Twist-lock
NUM_TABS        = 3       # number of bayonet tabs (evenly spaced)
TAB_ANG_DEG     = 30.0    # angular width of each tab (degrees)
TAB_DEPTH       = 5.0     # radial protrusion of tab (mm)
TAB_HEIGHT      = 6.0     # axial height of tab (mm)
LOCK_ROT_DEG    = 40.0    # CCW rotation to lock (degrees)
TAB_CLEAR_R     = 0.3     # radial clearance tab/slot (mm)
TAB_CLEAR_ANG   = 2.0     # angular clearance each side of tab (degrees)
SLOT_ABOVE_H    = 2.0     # slot height above lock groove (entry guide, mm)
DETENT_R          = 1.4              # detent bump radius (mm)
DETENT_H          = TAB_HEIGHT + 0.5  # detent bump height from channel floor (mm)
DETENT_ANG_OFFSET = 5.0              # degrees inward from CCW corner toward tab centre (increase to move left)

# ── Derived ───────────────────────────────────────────────────────────────────
INNER_R      = CYLINDER_DIAM / 2
OUTER_R      = INNER_R + WALL
BOX_H        = CYLINDER_HEIGHT + FLOOR
CLEARANCE    = 0.3
PLUG_R       = INNER_R - CLEARANCE       # plug outer radius (slides in box)
GROOVE_H     = TAB_HEIGHT + 0.5          # groove axial height (tab + clearance)
GROOVE_Z     = BOX_H - SLOT_ABOVE_H - GROOVE_H   # groove floor Z  (77.5 mm)
GROOVE_TOP   = BOX_H - SLOT_ABOVE_H              # groove ceiling Z (81 mm)
PLUG_DEPTH   = SLOT_ABOVE_H + GROOVE_H            # plug depth (tabs reach groove)
SLOT_R_OUT   = INNER_R + TAB_DEPTH + TAB_CLEAR_R  # slot outer radius (47.3 mm)
TAB_R_OUT    = PLUG_R + TAB_DEPTH                  # tab outer radius (46.7 mm)
LID_ROT_DEG  = 45.0 - (LOCK_ROT_DEG % 45.0)       # pre-rotate lid so edges align when locked (= 5° for 40° lock)

# ── Utilities ─────────────────────────────────────────────────────────────────

def mm(v):
    return v / 10.0


def draw_octagon(sketch, inradius, angle_offset_deg=0):
    R   = inradius / math.cos(math.pi / 8)
    off = math.radians(angle_offset_deg)
    lines = sketch.sketchCurves.sketchLines
    pts = [(R * math.cos(math.pi / 8 + off + i * math.pi / 4),
            R * math.sin(math.pi / 8 + off + i * math.pi / 4)) for i in range(8)]
    for i in range(8):
        x0, y0 = pts[i]; x1, y1 = pts[(i + 1) % 8]
        lines.addByTwoPoints(
            adsk.core.Point3D.create(mm(x0), mm(y0), 0),
            adsk.core.Point3D.create(mm(x1), mm(y1), 0))


def make_plane(root, base, offset_mm):
    inp = root.constructionPlanes.createInput()
    inp.setByOffset(base, adsk.core.ValueInput.createByReal(mm(offset_mm)))
    return root.constructionPlanes.add(inp)


def dist_val(h_mm):
    return adsk.fusion.DistanceExtentDefinition.create(
        adsk.core.ValueInput.createByReal(mm(h_mm)))


_POS  = adsk.fusion.ExtentDirections.PositiveExtentDirection
_NEG  = adsk.fusion.ExtentDirections.NegativeExtentDirection
_NEW  = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
_CUT  = adsk.fusion.FeatureOperations.CutFeatureOperation
_JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation


def extrude(root, profile, h_mm, op, direction=_POS, bodies=None):
    ei = root.features.extrudeFeatures.createInput(profile, op)
    ei.setOneSideExtent(dist_val(h_mm), direction)
    if bodies:
        ei.participantBodies = bodies
    return root.features.extrudeFeatures.add(ei)


def rect_profile(sketch, width, height):
    lines = sketch.sketchCurves.sketchLines
    hw, hh = mm(width / 2), mm(height / 2)
    corners = [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)]
    for i in range(4):
        ax, ay = corners[i]; bx, by = corners[(i + 1) % 4]
        lines.addByTwoPoints(
            adsk.core.Point3D.create(ax, ay, 0),
            adsk.core.Point3D.create(bx, by, 0))
    return sketch.profiles.item(0)


def extrude_sector(root, base_plane, r_in, r_out, a0_deg, a1_deg,
                   h_mm, op, direction=_POS, bodies=None):
    """Extrude an annular sector CCW from a0_deg to a1_deg."""
    a0    = math.radians(a0_deg)
    a1    = math.radians(a1_deg)
    sweep = a1 - a0
    sk    = root.sketches.add(base_plane)
    arcs  = sk.sketchCurves.sketchArcs
    lns   = sk.sketchCurves.sketchLines
    org   = adsk.core.Point3D.create(0, 0, 0)

    def pt(r, a):
        return adsk.core.Point3D.create(mm(r * math.cos(a)),
                                        mm(r * math.sin(a)), 0)
    pi0, pi1 = pt(r_in,  a0), pt(r_in,  a1)
    po0, po1 = pt(r_out, a0), pt(r_out, a1)

    arcs.addByCenterStartSweep(org, pi0, sweep)   # inner arc
    arcs.addByCenterStartSweep(org, po0, sweep)   # outer arc
    lns.addByTwoPoints(pi0, po0)                   # radial line at a0
    lns.addByTwoPoints(pi1, po1)                   # radial line at a1

    if sk.profiles.count == 0:
        return None
    return extrude(root, sk.profiles.item(0), h_mm, op, direction, bodies)


# ── Main ──────────────────────────────────────────────────────────────────────

def run(context):
    ui = None
    try:
        app  = adsk.core.Application.get()
        ui   = app.userInterface
        des  = adsk.fusion.Design.cast(app.activeProduct)
        root = des.rootComponent
        xy   = root.xYConstructionPlane
        yz   = root.yZConstructionPlane

        # ── 1. BOX OUTER SHELL ────────────────────────────────────────────────
        sk1 = root.sketches.add(xy)
        draw_octagon(sk1, OUTER_R)
        box_feat = extrude(root, sk1.profiles.item(0), BOX_H, _NEW)
        box_body = box_feat.bodies.item(0)
        box_body.name = "BoxBody"

        # ── 2. INNER CYLINDER CAVITY ──────────────────────────────────────────
        fl_pl = make_plane(root, xy, FLOOR)
        sk2   = root.sketches.add(fl_pl)
        sk2.sketchCurves.sketchCircles.addByCenterRadius(
            adsk.core.Point3D.create(0, 0, 0), mm(INNER_R))
        extrude(root, sk2.profiles.item(0), CYLINDER_HEIGHT, _CUT, _POS, [box_body])

        # ── 3. TWIST-LOCK SLOTS IN BOX INNER WALL ────────────────────────────
        # Each tab i gets two cuts forming an L-slot:
        #   a) Entry slot  : annular sector at entry angle, from GROOVE_Z to BOX_H
        #   b) Lock channel: annular sector CCW of entry, from GROOVE_Z to GROOVE_TOP
        #
        # hw = slot half-width (tab half-width + angular clearance each side)
        # chan goes from entry-slot CCW edge to LOCK_ROT_DEG further CCW.
        slot_pl = make_plane(root, xy, GROOVE_Z)
        entry_h = BOX_H - GROOVE_Z    # = SLOT_ABOVE_H + GROOVE_H
        hw      = TAB_ANG_DEG / 2 + TAB_CLEAR_ANG

        for i in range(NUM_TABS):
            ctr = i * 360.0 / NUM_TABS

            # a) Entry slot (full slot height)
            extrude_sector(root, slot_pl,
                           INNER_R, SLOT_R_OUT,
                           ctr - hw, ctr + hw,
                           entry_h, _CUT, _POS, [box_body])

            # b) Lock channel (groove height only, tab slides here when rotated)
            chan_s = ctr + hw
            chan_e = ctr + LOCK_ROT_DEG + hw
            extrude_sector(root, slot_pl,
                           INNER_R, SLOT_R_OUT,
                           chan_s, chan_e,
                           GROOVE_H, _CUT, _POS, [box_body])

            # c) Detent bump: against inner wall, at CCW edge of locked tab position.
            # Angularly at tab's CCW edge when locked (ctr + LOCK_ROT + TAB_ANG/2)
            # = just left of tab centre, pressed against the end wall.
            # Radially: INNER_R + DETENT_R → bump touches the inner cylindrical wall.
            lock_ang = math.radians(ctr + LOCK_ROT_DEG + TAB_ANG_DEG / 2 - DETENT_ANG_OFFSET)
            r_mid    = SLOT_R_OUT  # center on outer wall → half bump visible, half in wall
            sk_det   = root.sketches.add(slot_pl)
            sk_det.sketchCurves.sketchCircles.addByCenterRadius(
                adsk.core.Point3D.create(mm(r_mid * math.cos(lock_ang)),
                                         mm(r_mid * math.sin(lock_ang)), 0),
                mm(DETENT_R))
            extrude(root, sk_det.profiles.item(0), DETENT_H, _JOIN, _POS, [box_body])

        # ── 4. SIDE HOLES ─────────────────────────────────────────────────────
        # YZ plane: sketch-X = world-Y (lateral), sketch-Y = world-Z (height)
        # Point3D(mm(HOLE_*_Y), mm(z_centre), 0)
        hz_small = HOLE_SMALL_BOT + HOLE_SMALL_DIAM / 2
        hz_large = HOLE_LARGE_BOT + HOLE_LARGE_DIAM / 2
        cut_dist = 2 * OUTER_R + 5

        sk4 = root.sketches.add(yz)
        sk4.sketchCurves.sketchCircles.addByCenterRadius(
            adsk.core.Point3D.create(mm(HOLE_SMALL_Y), mm(hz_small), 0),
            mm(HOLE_SMALL_DIAM / 2))
        extrude(root, sk4.profiles.item(0), cut_dist, _CUT, _POS, [box_body])

        sk5 = root.sketches.add(yz)
        sk5.sketchCurves.sketchCircles.addByCenterRadius(
            adsk.core.Point3D.create(mm(HOLE_LARGE_Y), mm(hz_large), 0),
            mm(HOLE_LARGE_DIAM / 2))
        extrude(root, sk5.profiles.item(0), cut_dist, _CUT, _NEG, [box_body])

        # ── 5. LID DISC ───────────────────────────────────────────────────────
        lid_pl = make_plane(root, xy, BOX_H + LID_GAP)
        sk_lid = root.sketches.add(lid_pl)
        draw_octagon(sk_lid, OUTER_R, LID_ROT_DEG)
        lid_feat = extrude(root, sk_lid.profiles.item(0), LID_THICKNESS, _NEW)
        lid_body = lid_feat.bodies.item(0)
        lid_body.name = "Lid"

        # ── 6. LID PLUG ───────────────────────────────────────────────────────
        # Solid cylinder, radius PLUG_R, hangs downward PLUG_DEPTH from lid.
        # In model space plug bottom is LID_GAP above box, so no body overlap.
        sk_plug = root.sketches.add(lid_pl)
        sk_plug.sketchCurves.sketchCircles.addByCenterRadius(
            adsk.core.Point3D.create(0, 0, 0), mm(PLUG_R))
        extrude(root, sk_plug.profiles.item(0), PLUG_DEPTH, _JOIN, _NEG)

        # ── 7. BAYONET TABS ON PLUG ───────────────────────────────────────────
        # N sectors at entry angles, from PLUG_R to TAB_R_OUT, TAB_HEIGHT tall.
        # They sit at plug bottom → when lid pressed in, tabs sit in groove.
        tab_pl = make_plane(root, lid_pl, -PLUG_DEPTH)
        tab_hw = TAB_ANG_DEG / 2   # tab half-width (narrower than slot by TAB_CLEAR_ANG)

        for i in range(NUM_TABS):
            ctr = i * 360.0 / NUM_TABS
            extrude_sector(root, tab_pl,
                           PLUG_R, TAB_R_OUT,
                           ctr - tab_hw, ctr + tab_hw,
                           TAB_HEIGHT, _JOIN, _POS)

            # Detent dimple on tab bottom face at the CCW outer corner.
            # Model-space angle = ctr + TAB_ANG_DEG/2 (CCW edge of tab in entry
            # position). After lid rotates LOCK_ROT_DEG to lock, this corner
            # reaches the box bump angle → bump engages dimple → click.
            # r = TAB_R_OUT so half-circle is inside tab, half in clearance gap.
            dim_ang = math.radians(ctr + TAB_ANG_DEG / 2 - DETENT_ANG_OFFSET)
            sk_dim  = root.sketches.add(tab_pl)
            sk_dim.sketchCurves.sketchCircles.addByCenterRadius(
                adsk.core.Point3D.create(
                    mm(TAB_R_OUT * math.cos(dim_ang)),
                    mm(TAB_R_OUT * math.sin(dim_ang)), 0),
                mm(DETENT_R))
            extrude(root, sk_dim.profiles.item(0), DETENT_H, _CUT, _POS, [lid_body])

        # ── 8. LID SLOT ───────────────────────────────────────────────────────
        lid_top_pl = make_plane(root, lid_pl, LID_THICKNESS)
        sk_slot    = root.sketches.add(lid_top_pl)
        sp         = rect_profile(sk_slot, LID_SLOT_W, LID_SLOT_H)
        extrude(root, sp, LID_THICKNESS+PLUG_DEPTH, _CUT, _NEG, [lid_body])

        # ── Done ──────────────────────────────────────────────────────────────
        app.activeViewport.refresh()
        ui.messageBox(
            "Octagonal box — twist-lock lid complete!\n\n"
            f"Cavity:     ø{CYLINDER_DIAM} × {CYLINDER_HEIGHT} mm\n"
            f"Small hole: ø{HOLE_SMALL_DIAM} mm (+X, bottom at {HOLE_SMALL_BOT} mm)\n"
            f"Large hole: ø{HOLE_LARGE_DIAM} mm (-X, bottom at {HOLE_LARGE_BOT} mm)\n"
            f"Tabs:       {NUM_TABS} × {TAB_ANG_DEG}° wide, "
            f"{TAB_DEPTH} mm deep, {TAB_HEIGHT} mm tall\n"
            f"Lock:       rotate {LOCK_ROT_DEG}° CCW to lock\n"
            f"Lid slot:   {LID_SLOT_W} × {LID_SLOT_H} mm\n\n"
            f"Move lid down {LID_GAP} mm onto box to check fit."
        )

    except Exception:
        if ui:
            ui.messageBox("Script failed:\n" + traceback.format_exc())
