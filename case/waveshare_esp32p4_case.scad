// =============================================================
// Waveshare ESP32-P4 5" Touch LCD — druckbares Gehaeuse
// =============================================================
// Erzeugt Base + Lid nebeneinander.
// Verschraubung: 4x lange M2.5 von UNTEN durch Base, durchs PCB,
// in Self-Tap-Bosse im Lid. Eine Schraube haelt alles zusammen.
// =============================================================

// ----------- PARAMETER (alle in MILLIMETER) -----------

// Was rendern? "base", "lid", oder "both"
PART = "base";

// --- Waveshare ESP32-P4 5" Board ---
PCB_W          = 126.9;
PCB_H          = 70.7;
PCB_THICK      = 1.6;
PCB_BACK_CLEAR = 10.0;   // Bauteilhoehe auf PCB-Rueckseite

// PCB-Befestigungsloecher (Datenblatt S.1: Lochabstand 112 x 57 mm,
// 10.1 mm vom linken Glasrand, Y mittig zum Glas)
HOLE_X_OFF     = 10.1;
HOLE_DIST_X    = 112.0;
HOLE_DIST_Y    = 57.0;
HOLE_Y_OFF     = (PCB_H - HOLE_DIST_Y) / 2;   // = 6.85 mm
HOLE_DIA       = 2.5;

// USB-C Ports (Datenblatt S.4 Seitenansicht des Boards):
// USB TO UART (oben):  Mitte = 70.7 - 23.27 = 47.43 mm vom unteren Glasrand
// USB OTG     (unten): Mitte = 23.27 mm vom unteren Glasrand
// Abstand der beiden Ports: 17.8 mm (Datenblatt S.1)
USB_W          = 9.0;
USB_H          = 3.2;
USB1_Y_CENTER  = 23.27;
USB2_Y_CENTER  = 70.7 - 23.27;
USB_Z_OFFSET   = 5.0;    // Hoehe Buchsen-Mitte ueber PCB-Oberseite

// --- Gehaeuse ---
WALL           = 1.2;    // Wandstaerke (3 Perimeter @ 0.4 Duese)
STANDOFF_H     = 12.0;   // PCB-Standoff-Hoehe ueber Innenboden
                         // muss >= PCB_BACK_CLEAR sein, damit Komponenten
                         // auf der PCB-Rueckseite nicht den Boden beruehren
STANDOFF_OD    = 8.0;    // Aussendurchmesser (breite Auflage)
STANDOFF_TOP_OD = 6.0;   // Oberer Teller (PCB-Auflage)
STANDOFF_TOP_H = 1.5;    // Hoehe des Auflagetellers
CLEARANCE      = 0.4;
DISPLAY_BEZEL  = 3.0;    // Rahmen ums Displayfenster

// PCB-Passung
PCB_FIT_GAP    = 0.3;    // Luft links + Y zwischen PCB und Innenwand
// rechts (USB-Seite) liegt das PCB direkt an der Wand

// Klemmring im Lid
CLAMP_RIB_W    = 2.0;
CLAMP_RIB_H    = 1.0;

// Schraubenkopf-Senkbohrung im Aussenboden (DIN 7991 M2.5 Senkkopf)
HEAD_DIA       = 4.7;    // Kopf-Durchmesser oben
HEAD_DEPTH     = 1.5;    // konische Senkung, Tiefe
SHAFT_DIA      = 2.8;    // Schaft-Durchgang (M2.5 + 0.3 mm Spiel)

// Belueftungsschlitze in den langen Seitenwaenden
VENT_COUNT     = 8;      // Anzahl Schlitze pro Wand
VENT_W         = 2.0;    // Schlitzbreite (in X-Richtung)
VENT_H         = 8.0;    // Schlitzhoehe (in Z-Richtung)
VENT_SPACING   = 7.0;    // Mitte-zu-Mitte Abstand

// Abstand Base <-> Lid in Render-Vorschau
SPLIT_GAP      = 20.0;

// Render-Aufloesung
$fn = 64;


// ---------------- ABGELEITETE WERTE ----------------

INNER_PAD_L    = PCB_FIT_GAP;     // links
INNER_PAD_R    = 0.0;             // rechts (USB-Seite)
INNER_PAD_Y    = PCB_FIT_GAP;     // oben/unten

INNER_W        = PCB_W + INNER_PAD_L + INNER_PAD_R;
INNER_H        = PCB_H + 2*INNER_PAD_Y;

INNER_D_BASE   = PCB_BACK_CLEAR + STANDOFF_H + 2.0;
INNER_D_LID    = USB_Z_OFFSET + USB_H + 2.0;

OUTER_W        = INNER_W + 2*WALL;
OUTER_H        = INNER_H + 2*WALL;
BASE_OUTER_D   = INNER_D_BASE + WALL;
LID_OUTER_D    = INNER_D_LID + WALL;

// PCB-Origin im Innenraum (links-unten)
PCB_OX         = WALL + INNER_PAD_L;
PCB_OY         = WALL + INNER_PAD_Y;

// PCB-Hole-Positionen (Welt-Koordinaten, beide Teile teilen sich diese)
HOLES = [
    [PCB_OX + HOLE_X_OFF,               PCB_OY + HOLE_Y_OFF],
    [PCB_OX + HOLE_X_OFF,               PCB_OY + HOLE_Y_OFF + HOLE_DIST_Y],
    [PCB_OX + HOLE_X_OFF + HOLE_DIST_X, PCB_OY + HOLE_Y_OFF],
    [PCB_OX + HOLE_X_OFF + HOLE_DIST_X, PCB_OY + HOLE_Y_OFF + HOLE_DIST_Y],
];


// ====================== BASE ======================

module case_base() {
    difference() {
        union() {
            // ---------- Wanne (Aussenklotz minus Innenraum) ----------
            difference() {
                cube([OUTER_W, OUTER_H, BASE_OUTER_D]);
                // Innenraum aushoehlen
                translate([WALL, WALL, WALL])
                    cube([INNER_W, INNER_H, INNER_D_BASE + 1]);
            }

            // ---------- Standoffs (PCB-Auflage) ----------
            // Breiter Sockel unten, schmaler Auflageteller oben.
            for (p = HOLES) {
                translate([p[0], p[1], WALL])
                    cylinder(h = STANDOFF_H - STANDOFF_TOP_H,
                             d = STANDOFF_OD);
                translate([p[0], p[1], WALL + STANDOFF_H - STANDOFF_TOP_H])
                    cylinder(h = STANDOFF_TOP_H, d = STANDOFF_TOP_OD);
            }

            // ---------- Stuetzrippen Standoff -> Wand ----------
            RIB_T = 1.6;
            RIB_H = STANDOFF_H - STANDOFF_TOP_H;
            for (p = HOLES) {
                left_d  = p[0] - WALL;
                right_d = OUTER_W - WALL - p[0];
                bot_d   = p[1] - WALL;
                top_d   = OUTER_H - WALL - p[1];

                if (left_d <= right_d)
                    translate([WALL, p[1] - RIB_T/2, WALL])
                        cube([left_d, RIB_T, RIB_H]);
                else
                    translate([p[0], p[1] - RIB_T/2, WALL])
                        cube([right_d, RIB_T, RIB_H]);

                if (bot_d <= top_d)
                    translate([p[0] - RIB_T/2, WALL, WALL])
                        cube([RIB_T, bot_d, RIB_H]);
                else
                    translate([p[0] - RIB_T/2, p[1], WALL])
                        cube([RIB_T, top_d, RIB_H]);
            }
        }

        // ---------- USB-Schlitze in der rechten Wand ----------
        for (yc = [USB1_Y_CENTER, USB2_Y_CENTER]) {
            translate([OUTER_W - WALL - 0.5,
                       PCB_OY + yc - (USB_W + CLEARANCE)/2,
                       WALL + STANDOFF_H + PCB_THICK + USB_Z_OFFSET
                         - (USB_H + CLEARANCE)/2])
                cube([WALL + 1.0,
                      USB_W + CLEARANCE,
                      USB_H + CLEARANCE + 100]);
        }

        // ---------- Schraubenkanaele in den Standoffs ----------
        // Aussen: Senkkopf-Senkung, dann Schaft-Spiel im unteren Teil,
        // oben Self-Tap-Kernloch im PCB-nahen Bereich.
        PILOT_DIA = HOLE_DIA - 0.45;
        SHAFT_RUN = WALL + STANDOFF_H - 5.0;
        for (p = HOLES) {
            translate([p[0], p[1], -0.01])
                cylinder(h = HEAD_DEPTH + 0.01,
                         d1 = HEAD_DIA, d2 = SHAFT_DIA);
            translate([p[0], p[1], HEAD_DEPTH])
                cylinder(h = SHAFT_RUN - HEAD_DEPTH, d = SHAFT_DIA);
            translate([p[0], p[1], SHAFT_RUN])
                cylinder(h = STANDOFF_H + WALL - SHAFT_RUN + 0.5,
                         d = PILOT_DIA);
        }

        // ---------- Belueftungsschlitze in den langen Seitenwaenden ----------
        vent_z = WALL + (STANDOFF_H - VENT_H) / 2;
        vent_total_w = (VENT_COUNT - 1) * VENT_SPACING + VENT_W;
        vent_x_start = (OUTER_W - vent_total_w) / 2;
        for (i = [0:VENT_COUNT-1]) {
            x = vent_x_start + i * VENT_SPACING;
            translate([x, -0.5, vent_z])
                cube([VENT_W, WALL + 1, VENT_H]);
            translate([x, OUTER_H - WALL - 0.5, vent_z])
                cube([VENT_W, WALL + 1, VENT_H]);
        }

        // ---------- Belueftungsschlitze im Boden ----------
        floor_vent_count_x = 4;
        floor_vent_count_y = 3;
        floor_vent_w = 3.0;
        floor_vent_l = 18.0;
        floor_inset = 18.0;
        fv_area_w = OUTER_W - 2*floor_inset;
        fv_area_h = OUTER_H - 2*floor_inset;
        fv_dx = fv_area_w / (floor_vent_count_x - 1);
        fv_dy = fv_area_h / (floor_vent_count_y - 1);
        for (ix = [0:floor_vent_count_x-1]) {
            for (iy = [0:floor_vent_count_y-1]) {
                cx = floor_inset + ix * fv_dx;
                cy = floor_inset + iy * fv_dy;
                min_d = min([for (p = HOLES)
                    sqrt(pow(cx - p[0], 2) + pow(cy - p[1], 2))]);
                if (min_d > STANDOFF_OD/2 + floor_vent_l/2 + 3)
                    translate([cx - floor_vent_l/2,
                               cy - floor_vent_w/2,
                               -0.5])
                        cube([floor_vent_l, floor_vent_w, WALL + 1]);
            }
        }
    }
}


// ====================== LID ======================

module case_lid() {
    difference() {
        union() {
            // Aussen-Klotz
            cube([OUTER_W, OUTER_H, LID_OUTER_D]);

            // Klemmrippe innen (drueckt Glas runter)
            translate([WALL, WALL, WALL])
                difference() {
                    cube([INNER_W, INNER_H, CLAMP_RIB_H]);
                    translate([CLAMP_RIB_W, CLAMP_RIB_W, -0.5])
                        cube([INNER_W - 2*CLAMP_RIB_W,
                              INNER_H - 2*CLAMP_RIB_W,
                              CLAMP_RIB_H + 1]);
                }

            // Lid-Bosse fuer Self-Tap M2.5
            lid_boss_h = INNER_D_LID - PCB_THICK - 1.0;
            for (p = HOLES)
                translate([p[0], p[1], WALL])
                    cylinder(h = lid_boss_h, d = STANDOFF_OD);
        }

        // Innenraum aushoehlen
        translate([WALL, WALL, WALL])
            cube([INNER_W, INNER_H, INNER_D_LID + 1]);

        // Displayfenster (komplett durch)
        translate([PCB_OX + DISPLAY_BEZEL,
                   PCB_OY + DISPLAY_BEZEL,
                   -0.5])
            cube([PCB_W - 2*DISPLAY_BEZEL,
                  PCB_H - 2*DISPLAY_BEZEL,
                  LID_OUTER_D + 1]);

        // Kernloecher in den Bossen fuer Self-Tap M2.5
        for (p = HOLES)
            translate([p[0], p[1], WALL])
                cylinder(h = INNER_D_LID, d = HOLE_DIA - 0.45);
    }
}


// ====================== RENDER ======================

if (PART == "base" || PART == "both") {
    case_base();
}

if (PART == "lid" || PART == "both") {
    translate([OUTER_W + SPLIT_GAP, 0, 0])
        case_lid();
}
