// ============================================================
// BOITIER ESP32-4827S043 + Batterie 10000mAh + Fixation murale
// Moniteur multi-cameras — version "tablette murale detachable"
// ============================================================
// 3 pieces a imprimer :
//   part=1 -> corps (cadre ecran + logement carte + batterie)
//   part=2 -> couvercle arriere (avec rail femelle)
//   part=3 -> plaque murale (avec rail male, a visser au mur)
//
// Le boitier glisse de haut en bas sur la plaque murale et se
// detache facilement pour etre utilise comme tablette.
// ============================================================

part = 0;   // 0 = vue eclatee, 1/2/3 = piece a exporter

// ---------- MESURES A AJUSTER (mm) ----------
pcb_w = 105.5;   // largeur carte 4827S043
pcb_h = 74.0;    // hauteur carte
pcb_d = 12.0;    // epaisseur carte+ecran+composants
scr_w = 95.5;    // fenetre ecran visible (95.04 + jeu)
scr_h = 54.5;    // (53.86 + jeu)
scr_offx = 0;    // decalage fenetre ecran X si besoin
scr_offy = 0;    // decalage fenetre ecran Y si besoin

// Batterie 10000mAh plate LiPo : MESURE LA TIENNE et ajuste !
bat_w = 101.0;   // longueur batterie
bat_h = 61.0;    // largeur batterie
bat_d = 13.0;    // epaisseur batterie

wall   = 2.0;    // epaisseur parois laterales
front  = 2.2;    // epaisseur face avant (cadre)
gap    = 0.4;    // jeu d'assemblage carte

// ---------- dimensions calculees ----------
ext_w = pcb_w + 2*wall + gap;          // largeur externe boitier
ext_h = pcb_h + 2*wall + gap;          // hauteur externe
depth = front + pcb_d + bat_d + wall;  // profondeur totale ~29mm

// ---------- rail queue d'aronde (murale) ----------
rail_len  = 60;   // longueur du rail (glissement vertical)
rail_wb   = 26;   // largeur a la base (contre la plaque)
rail_wt   = 17;   // largeur au sommet
rail_h    = 6;    // hauteur du rail
rail_play = 0.35; // jeu coulissant

// ---------- vis ----------
boss_d  = 6;      // diametre boss de fixation couvercle
screw_d = 2.2;    // trou vis M2 auto-taraudeuse
mwall_d = 4.5;    // trous vis murale (vis bois 4mm)

$fn = 48;

// ============================================================
module corps() {
  difference() {
    union() {
      translate([-ext_w/2, -ext_h/2, 0])
        cube([ext_w, ext_h, depth - wall]);
      for (x = [-1, 1], y = [-1, 1])
        translate([x*(ext_w/2 - wall - boss_d/2 - 1),
                   y*(ext_h/2 - wall - boss_d/2 - 1), 0])
          cylinder(d = boss_d, h = depth - wall);
    }
    // cavite carte (derriere le cadre avant)
    translate([-pcb_w/2 - gap/2, -pcb_h/2 - gap/2, front])
      cube([pcb_w + gap, pcb_h + gap, pcb_d + 0.2]);
    // cavite batterie
    translate([-bat_w/2, -bat_h/2, front + pcb_d])
      cube([bat_w, bat_h, bat_d + 0.2]);
    // fenetre ecran
    translate([-scr_w/2 + scr_offx, -scr_h/2 + scr_offy, -0.1])
      cube([scr_w, scr_h, front + 0.2]);
    // ouverture USB-C (cote droit)
    translate([ext_w/2 - wall - 0.1, -8, front + pcb_d/2 - 3])
      cube([wall + 0.2, 16, 6]);
    // trous acces boutons RESET / BOOT (cote gauche)
    for (y = [-6, 6])
      translate([-ext_w/2 - 0.1, y, front + pcb_d/2 - 1.5])
        rotate([0, 90, 0]) cylinder(d = 3, h = wall + 0.2);
    // trous vis couvercle dans les boss
    for (x = [-1, 1], y = [-1, 1])
      translate([x*(ext_w/2 - wall - boss_d/2 - 1),
                 y*(ext_h/2 - wall - boss_d/2 - 1), depth - wall - 8])
        cylinder(d = screw_d, h = 8.1);
    // fentes de ventilation laterales (2 grands cotes, zone carte)
    for (y = [-20, -10, 0, 10, 20]) {
      translate([ext_w/2 - wall - 0.1, y, front + pcb_d/2 - 1])
        cube([wall + 0.2, 5, 8]);
      translate([-ext_w/2 - 0.1, y, front + pcb_d/2 - 1])
        cube([wall + 0.2, 5, 8]);
    }
  }
}

// ============================================================
// Rainure femelle : ETROITE a l'ouverture, LARGE au fond
// (complementaire du rail male : large a la base, etroit au sommet)
module rail_femelle(l) {
  linear_extrude(height = l)
    polygon([[-rail_wt/2 - rail_play, 0],
             [-rail_wb/2 - rail_play, rail_h + rail_play],
             [ rail_wb/2 + rail_play, rail_h + rail_play],
             [ rail_wt/2 + rail_play, 0]]);
}

module couvercle() {
  joug_h = rail_h + rail_play + 1.2;
  difference() {
    union() {
      // plaque
      translate([-ext_w/2, -ext_h/2, 0])
        cube([ext_w, ext_h, wall]);
      // joug autour du rail femelle
      translate([-rail_wb/2 - 4, -rail_len/2, wall])
        cube([rail_wb + 8, rail_len, joug_h]);
    }
    // rainure femelle traversante (le long de Y, profil dans XZ)
    translate([0, rail_len/2 + 0.1, wall])
      rotate([90, 0, 0])
        rail_femelle(rail_len + 0.2);
    // trous vis aux coins (alignes sur les boss du corps)
    for (x = [-1, 1], y = [-1, 1])
      translate([x*(ext_w/2 - wall - boss_d/2 - 1),
                 y*(ext_h/2 - wall - boss_d/2 - 1), -0.1])
        cylinder(d = screw_d + 0.6, h = wall + 0.2);
  }
}

// ============================================================
module rail_male(l) {
  // trapeze : base large (contre plaque), sommet etroit
  linear_extrude(height = l)
    polygon([[-rail_wb/2, 0], [-rail_wt/2, rail_h],
             [ rail_wt/2, rail_h], [ rail_wb/2, 0]]);
}

module plaque_murale() {
  pw = 70; ph = rail_len + 16; pt = 3;   // plaque 70 x 76 x 3
  difference() {
    union() {
      translate([-pw/2, -ph/2, 0])
        cube([pw, ph, pt]);
      // rail male COUCHE le long de Y (boitier glisse de haut en bas)
      translate([0, rail_len/2, pt])
        rotate([90, 0, 0])
          rail_male(rail_len);
      // butee basse (fin de course du boitier)
      translate([-rail_wt/2, -rail_len/2 - 4, pt])
        cube([rail_wt, 4, rail_h]);
    }
    // 4 trous de vis fraises pour fixation murale
    for (x = [-1, 1], y = [-1, 1])
      translate([x*(pw/2 - 8), y*(ph/2 - 8), -0.1]) {
        cylinder(d = mwall_d, h = pt + 0.2);
        translate([0, 0, pt - 1.6])
          cylinder(d1 = mwall_d, d2 = mwall_d*2.1, h = 1.7);   // fraisage
      }
  }
}

// ============================================================
// ---------- rendu ----------
if (part == 1) {
  corps();
} else if (part == 2) {
  couvercle();
} else if (part == 3) {
  plaque_murale();
} else {
  // vue eclatee
  corps();
  translate([0, 0, depth + 8]) couvercle();
  translate([0, 0, depth + 30]) plaque_murale();
}
