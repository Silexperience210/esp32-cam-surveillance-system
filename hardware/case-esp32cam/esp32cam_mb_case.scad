// =====================================================================
//  BOITIER ESP32-CAM + CARTE PROGRAMMATION ESP32-CAM-MB
//  Parois fines 1.6 mm - Texture stries verticales - Couvercle friction
//  IMPRESSION : corps + couvercle, aucun support, PLA/PETG 0.2 mm
// =====================================================================
//  ⚠️  COTES PAR DÉFAUT = montage standard AI-Thinker + MB.
//      MESURE ton empilage et ajuste les variables ci-dessous.
// =====================================================================

// ---------- CHOIX DE LA PIÈCE ----------
part = 3;        // 1 = corps | 2 = couvercle | 3 = les deux (impression) | 4 = éclaté (vérif)

// ---------- COTES INTERNES (À MESURER) ----------
in_l = 54.5;     // longueur interne  (carte MB ≈ 52.5 + jeu)
in_w = 28.2;     // largeur interne   (carte MB ≈ 26   + jeu)
in_h = 17.0;     // hauteur interne   (empilage MB + ESP32-CAM + nappe)

wall   = 1.6;    // épaisseur des parois  ← ta demande
corner = 3.0;    // rayon des coins extérieurs

// ---------- CAMÉRA (face avant) ----------
cam_z  = 14.5;   // hauteur du centre de la lentille (depuis le fond interne)
cam_y  = 0;      // décalage latéral lentille (0 = centrée)
lens_d = 11;     // diamètre fenêtre lentille

// ---------- LED FLASH GPIO4 ----------
top_flash   = true;    // fenêtre dans le COUVERCLE (LED AI-Thinker émet vers le haut)
front_flash = false;   // fenêtre dans la face avant (si LED orientée avant)
flash_x  = 20;         // position X de la LED (vers l'avant, côté connecteur cam)
flash_y  = -8.5;       // position Y de la LED
flash_d  = 6.5;        // diamètre fenêtre flash

// ---------- USB (face arrière) ----------
usb_w = 13;      // largeur fenêtre micro-USB
usb_h = 9;       // hauteur fenêtre
usb_z = 5;       // hauteur du centre (connecteur au bas de la MB)

// ---------- OPTIONS ----------
vent         = true;   // fentes de ventilation sur les côtés
sd_slot      = true;   // fente carte microSD (paroi gauche)
sd_x   = 5;  sd_z = 12.3;  sd_len = 15;  sd_h = 2.6;   // position fente SD
reset_hole   = true;   // trou d'aiguille pour bouton reset MB (couvercle)
reset_x = -20;  reset_y = 7;  reset_d = 3;
ant_window   = true;   // fenêtre antenne WiFi (couvercle, améliore le signal)
ant_x = -14; ant_w = 15; ant_h = 12;
mount_holes  = true;   // 2 trous M3 dans le fond (fixation)
mount_spacing = 30;

// ---------- COUVERCLE ----------
lid_lip = 2.6;   // hauteur de la lèvre d'emboîtement
lid_clr = 0.25;  // jeu friction (±0.05 selon imprimante — resserre si trop lâche)

// ---------- TEXTURE STRIES ----------
rib      = true;
rib_step = 2.1;  // espacement des stries (mm)
rib_r    = 0.85; // rayon des stries = profondeur du relief

// =====================================================================
//  CALCULS
// =====================================================================
out_l  = in_l + 2*wall;
out_w  = in_w + 2*wall;
body_h = in_h + wall;           // hauteur du corps (fond compris)

// =====================================================================
//  MODULES DE BASE
// =====================================================================
// Profil 2D arrondi du boîtier ; d>0 grossit, d<0 rétrécit
module profil(d=0) {
  offset(r = corner + d)
    square([out_l - 2*corner, out_w - 2*corner], center = true);
}

// Coque du corps (bac ouvert)
module coque() {
  difference() {
    linear_extrude(body_h) profil(0);
    translate([0, 0, wall])
      linear_extrude(body_h) profil(-wall);
  }
}

// Rails de support de la carte MB (évite les soudures des headers)
module rails() {
  for (s = [-1, 1])
    translate([0, s * 10, wall])
      cube([48, 3, 1.2], center = true);
}

// Stries verticales : cylindres posés EXACTEMENT sur le périmètre
// (segments droits + quarts de cercle aux coins -> ancrage garanti)
rl = out_l/2 - corner;      // demi-longueur des parties droites
rw = out_w/2 - corner;
nl = floor(rl / rib_step);  // nb stries par demi-face longue
nw = floor(rw / rib_step);  // nb stries par demi-face courte
na = max(4, round(PI*corner/2 / rib_step)); // stries par quart de coin

module strie(x, y, h)
  translate([x, y, -0.1]) cylinder(r = rib_r, h = h + 0.2, $fn = 10);

module stries(h) {
  intersection() {
    union() {
      // faces longues (y = ±out_w/2)
      for (s = [-1, 1], i = [-nl : nl])
        strie(i * rib_step, s * out_w/2, h);
      // faces courtes (x = ±out_l/2)
      for (s = [-1, 1], i = [-nw : nw])
        strie(s * out_l/2, i * rib_step, h);
      // quarts de cercle aux 4 coins
      for (cx = [-1, 1], cy = [-1, 1], a = [0 : na])
        strie(cx * (rl + corner * cos(a * 90 / na)),
              cy * (rw + corner * sin(a * 90 / na)), h);
    }
    // on ne garde que ce qui dépasse de la surface (ancrage 0.5 mm)
    difference() {
      linear_extrude(h + 0.2) profil(rib_r * 0.95);
      translate([0, 0, -0.1]) linear_extrude(h + 0.4) profil(-0.5);
    }
  }
}

// =====================================================================
//  OUVERTURES DU CORPS
// =====================================================================
module ouvertures_corps() {
  // --- Fenêtre lentille (face avant +X) ---
  translate([out_l/2, cam_y, cam_z])
    rotate([0, 90, 0])
      cylinder(d = lens_d, h = wall*3 + 1, center = true, $fn = 32);

  // --- Fenêtre flash LED face avant (option) ---
  if (front_flash)
    translate([out_l/2, flash_y, cam_z])
      rotate([0, 90, 0])
        cylinder(d = flash_d, h = wall*3 + 1, center = true, $fn = 24);

  // --- Fenêtre micro-USB (face arrière -X), oblongue ---
  translate([-out_l/2, 0, usb_z])
    rotate([0, 90, 0])
      hull()
        for (s = [-1, 1])
          translate([0, s * (usb_w - usb_h)/2, 0])
            cylinder(d = usb_h, h = wall*3 + 1, center = true, $fn = 24);

  // --- Fentes de ventilation (2 grands côtés, partie haute) ---
  if (vent)
    for (s = [-1, 1], j = [-1, 0, 1])
      translate([j * 14, s * out_w/2, body_h - 6])
        cube([2, wall*3 + 1, 7], center = true);

  // --- Fente carte microSD (paroi gauche -Y) ---
  if (sd_slot)
    translate([sd_x, -out_w/2, sd_z])
      cube([sd_len, wall*3 + 1, sd_h], center = true);

  // --- Trous de fixation M3 dans le fond ---
  if (mount_holes)
    for (s = [-1, 1])
      translate([0, s * mount_spacing/2, -0.5])
        cylinder(d = 3.2, h = wall + 1, $fn = 24);
}

// =====================================================================
//  CORPS
// =====================================================================
module corps() {
  difference() {
    union() {
      coque();
      rails();
      if (rib) stries(body_h);
    }
    ouvertures_corps();
  }
}

// =====================================================================
//  COUVERCLE (emboîtement friction)
// =====================================================================
module couvercle() {
  difference() {
    union() {
      // plaque
      linear_extrude(wall) profil(0);
      // lèvre d'emboîtement (rentre dans le corps)
      translate([0, 0, wall - 0.01])
        difference() {
          linear_extrude(lid_lip) profil(-wall - lid_clr);
          linear_extrude(lid_lip) profil(-wall - lid_clr - 1.1);
        }
      // stries sur le pourtour de la plaque
      if (rib)
        intersection() {
          stries(wall);
          linear_extrude(wall) profil(rib_r);
        }
    }
    // --- fenêtre flash LED (dessus) ---
    if (top_flash)
      translate([flash_x, flash_y, -0.5])
        cylinder(d = flash_d, h = wall + 1, $fn = 24);
    // --- trou bouton reset MB ---
    if (reset_hole)
      translate([reset_x, reset_y, -0.5])
        cylinder(d = reset_d, h = wall + 1, $fn = 20);
    // --- fenêtre antenne WiFi ---
    if (ant_window)
      translate([ant_x, 0, -0.5])
        hull()
          for (sx = [-1, 1], sy = [-1, 1])
            translate([sx * (ant_w/2 - 2), sy * (ant_h/2 - 2), 0])
              cylinder(r = 2, h = wall + 1, $fn = 16);
  }
}

// =====================================================================
//  RENDU
// =====================================================================
if (part == 1) corps();
else if (part == 2)
  translate([0, 0, wall + lid_lip]) rotate([180, 0, 0]) couvercle(); // à plat pour impression
else if (part == 3) {
  corps();
  translate([out_l/2 + 15 + out_l/2, 0, wall + lid_lip])
    rotate([180, 0, 0]) couvercle();
}
else if (part == 4) { // éclaté de vérification
  corps();
  translate([0, 0, body_h + 12]) couvercle();
}
