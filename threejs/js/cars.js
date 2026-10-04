/* =========================================================
 * cars.js — 路边停放车辆：AE86（熊猫豆腐店涂装）/ FC3S（白）
 * 下半身轮廓带轮拱切口（车轮可见），座舱玻璃带用整块多边形
 * 车头朝 +z
 * ========================================================= */
'use strict';
(function (App) {

  const GLASS = '#101218';
  const TRIM = '#22262e';
  const TIRE = '#14161b';

  const glassMat = App.toon(GLASS);
  glassMat.side = THREE.DoubleSide;
  const trimMat = App.toon(TRIM);
  const tireMat = App.toon(TIRE);
  const rimMat = App.toon('#b8bfc8');
  const rimMat2 = App.toon('#878d96');

  function extrude(shape, width, bevel) {
    const geo = new THREE.ExtrudeGeometry(shape, {
      depth: width, bevelEnabled: true,
      bevelThickness: bevel, bevelSize: bevel, bevelSegments: 2,
    });
    geo.translate(0, 0, -width / 2);
    geo.rotateY(-Math.PI / 2);   // 轮廓 +x(前) → +z(前)
    geo.computeVertexNormals();
    return geo;
  }

  /* 下半身：顶部轮廓 + 底部轮拱切口
   * topPoints 从后下角起沿顶部到前下角；wheels 为轮心轮廓坐标；archR 拱半径 */
  function lowerGeo(topPoints, wheels, archR, width, bevel) {
    const shape = new THREE.Shape();
    shape.moveTo(topPoints[0][0], topPoints[0][1]);
    for (let i = 1; i < topPoints.length; i++) shape.lineTo(topPoints[i][0], topPoints[i][1]);
    const y0 = topPoints[topPoints.length - 1][1];
    const rearX = topPoints[0][0];
    // 沿底部从前向后（-x）走，轮位处上拱
    const ws = [...wheels].sort((a, b) => b - a);
    for (const wz of ws) {
      shape.lineTo(wz + archR, y0);
      shape.absarc(wz, y0, archR, 0, Math.PI, false);
    }
    shape.lineTo(rearX, y0);
    shape.closePath();
    return extrude(shape, width, bevel);
  }

  /* 座舱：斜顶轮廓 */
  function cabinGeo(points, width, bevel) {
    const shape = new THREE.Shape();
    shape.moveTo(points[0][0], points[0][1]);
    for (let i = 1; i < points.length; i++) shape.lineTo(points[i][0], points[i][1]);
    shape.closePath();
    return extrude(shape, width, bevel);
  }

  /* 侧窗玻璃带：沿座舱侧面的整块多边形（profile 坐标，贴在 ±x 面上） */
  function sideGlass(points, x, mat) {
    // +x 面需镜像 u 再转 +90°；-x 面直接用原坐标转 -90°（保证法线朝外且 u→+z）
    const pts = x > 0 ? points.map(p => [-p[0], p[1]]) : points;
    const shape = new THREE.Shape();
    shape.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i++) shape.lineTo(pts[i][0], pts[i][1]);
    shape.closePath();
    const geo = new THREE.ShapeGeometry(shape);
    geo.rotateY(x > 0 ? Math.PI / 2 : -Math.PI / 2);
    const m = new THREE.Mesh(geo, mat);
    m.position.x = x;
    return m;
  }

  /* 斜面玻璃板（风挡/后窗），profile 坐标 (u1,v1)-(u2,v2)，u→z */
  function slopePanel(u1, v1, u2, v2, w, mat, out) {
    const du = u2 - u1, dv = v2 - v1;
    const len = Math.hypot(du, dv);
    let nz = -dv, ny = du;
    if (ny < 0) { nz = -nz; ny = -ny; }
    const nl = Math.hypot(nz, ny); nz /= nl; ny /= nl;
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, len), mat);
    m.rotation.x = Math.atan2(-ny, nz);
    const o = out || 0.02;
    m.position.set(0, (v1 + v2) / 2 + ny * o, (u1 + u2) / 2 + nz * o);
    return m;
  }

  function mkWheel(x, z, r, w, rim) {
    const g = new THREE.Group();
    const tire = new THREE.Mesh(new THREE.CylinderGeometry(r, r, w, 20), tireMat);
    tire.rotation.z = Math.PI / 2;
    g.add(tire); App.outline(tire, 0.014);
    const cap = new THREE.Mesh(new THREE.CylinderGeometry(r * 0.58, r * 0.58, w + 0.012, 20), rim);
    cap.rotation.z = Math.PI / 2;
    g.add(cap);
    const hub = new THREE.Mesh(new THREE.CylinderGeometry(r * 0.16, r * 0.16, w + 0.016, 10), tireMat);
    hub.rotation.z = Math.PI / 2;
    g.add(hub);
    g.position.set(x, r, z);
    return g;
  }

  const bx = (grp, w, h, d, mat, x, y, z, ow) => {
    const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), mat);
    m.position.set(x, y, z);
    grp.add(m);
    if (ow) App.outline(m, ow);
    return m;
  };

  /* 车底假接触阴影 */
  function carShadow(grp, w, l) {
    const tex = App.radialTex('rgba(0,0,0,0.68)', 'rgba(0,0,0,0)');
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, l), new THREE.MeshBasicMaterial({
      map: tex, transparent: true, opacity: 0.9, depthWrite: false,
    }));
    m.rotation.x = -Math.PI / 2;
    m.position.y = 0.006;
    m.renderOrder = 4;
    grp.add(m);
  }

  /* ================= AE86（白黑熊猫 / 藤原とうふ店） ================= */
  App.buildAE86 = function () {
    const car = new THREE.Group();
    const white = App.toon('#f0efe6');
    const WHEEL_Z = 0.58, WHEEL_R = 0.14;

    // 下半身（腰线/引擎盖/垂鼻 + 轮拱切口）
    const lower = new THREE.Mesh(lowerGeo([
      [-0.98, 0.07], [-0.98, 0.48],
      [0.55, 0.48],
      [0.88, 0.44],
      [0.98, 0.39],
      [0.98, 0.07],
    ], [WHEEL_Z, -WHEEL_Z], 0.17, 0.74, 0.012), white);
    lower.castShadow = true;
    car.add(lower); App.outline(lower, 0.016);

    // 座舱（风挡斜面 + 车顶 + 掀背斜面拉到车尾，无"车屁股"）
    const cabin = new THREE.Mesh(cabinGeo([
      [0.55, 0.42], [0.15, 0.64],
      [-0.42, 0.64],
      [-0.98, 0.485],
    ], 0.62, 0.012), white);
    cabin.castShadow = true;
    car.add(cabin); App.outline(cabin, 0.014);

    // 玻璃：风挡 / 后窗 / 两侧玻璃带（A柱→C柱连续）
    car.add(slopePanel(0.15, 0.64, 0.55, 0.42, 0.58, glassMat, 0.022));
    car.add(slopePanel(-0.42, 0.64, -0.96, 0.49, 0.56, glassMat, 0.022));
    const glassBand = [[0.53, 0.45], [0.16, 0.615], [-0.38, 0.615], [-0.93, 0.48]];
    car.add(sideGlass(glassBand, 0.328, glassMat));
    car.add(sideGlass(glassBand, -0.328, glassMat));

    // 熊猫元素：黑前后杠 + 侧防擦条
    bx(car, 0.79, 0.12, 0.22, trimMat, 0, 0.22, 0.93, 0.012);
    bx(car, 0.79, 0.12, 0.2, trimMat, 0, 0.22, -0.93, 0.012);
    bx(car, 0.795, 0.04, 1.86, trimMat, 0, 0.3, 0, 0);
    // 前进气黑缝 + 闭合跳灯盖板
    bx(car, 0.72, 0.025, 0.03, trimMat, 0, 0.345, 1.0, 0);
    const popL = bx(car, 0.2, 0.015, 0.24, trimMat, 0.23, 0.462, 0.72, 0);
    const popR = bx(car, 0.2, 0.015, 0.24, trimMat, -0.23, 0.462, 0.72, 0);
    popL.rotation.x = -0.12; popR.rotation.x = -0.12;
    // 转向灯（前杠面）
    bx(car, 0.13, 0.035, 0.02, App.glow('#d88a2e', 0.9), 0.3, 0.25, 1.045, 0);
    bx(car, 0.13, 0.035, 0.02, App.glow('#d88a2e', 0.9), -0.3, 0.25, 1.045, 0);
    // 尾部黑色饰带 + 尾灯
    bx(car, 0.68, 0.11, 0.03, trimMat, 0, 0.4, -1.0, 0.008);
    bx(car, 0.2, 0.08, 0.02, App.glow('#c8442e', 0.8), 0.18, 0.4, -1.02, 0);
    bx(car, 0.2, 0.08, 0.02, App.glow('#c8442e', 0.8), -0.18, 0.4, -1.02, 0);
    // 后视镜
    bx(car, 0.04, 0.05, 0.08, trimMat, 0.42, 0.5, 0.34, 0.008);
    bx(car, 0.04, 0.05, 0.08, trimMat, -0.42, 0.5, 0.34, 0.008);
    // 排气管
    const ex = new THREE.Mesh(new THREE.CylinderGeometry(0.028, 0.028, 0.12, 8), rimMat2);
    ex.rotation.x = Math.PI / 2;
    ex.position.set(-0.22, 0.12, -1.0);
    car.add(ex);

    // 车轮（在轮拱内可见）
    car.add(mkWheel(0.345, WHEEL_Z, WHEEL_R, 0.1, rimMat));
    car.add(mkWheel(-0.345, WHEEL_Z, WHEEL_R, 0.1, rimMat));
    car.add(mkWheel(0.345, -WHEEL_Z, WHEEL_R, 0.1, rimMat));
    car.add(mkWheel(-0.345, -WHEEL_Z, WHEEL_R, 0.1, rimMat));

    // 车门贴纸「藤原とうふ店」
    const tofuTex = App.canvasTex(256, 128, (ctx, w, h) => {
      ctx.clearRect(0, 0, w, h);
      ctx.fillStyle = '#181818';
      ctx.textAlign = 'center';
      ctx.font = `bold 46px ${App.FONT}`;
      ctx.fillText('藤原とうふ店', w / 2, 58);
      ctx.font = `bold 28px ${App.FONT}`;
      ctx.fillText('（自家用）', w / 2, 100);
    });
    for (const side of [1, -1]) {
      const decal = new THREE.Mesh(new THREE.PlaneGeometry(0.46, 0.23),
        new THREE.MeshBasicMaterial({ map: tofuTex, transparent: true, depthWrite: false }));
      decal.rotation.y = side * Math.PI / 2;
      decal.position.set(side * 0.397, 0.33, 0.03);
      decal.renderOrder = 3;
      car.add(decal);
    }

    carShadow(car, 1.25, 2.45);
    return car;
  };

  /* ================= FC3S RX-7（白色） ================= */
  App.buildFC = function () {
    const car = new THREE.Group();
    const white = App.toon('#eceae0');
    const WHEEL_Z = 0.64, WHEEL_R = 0.145;

    // 下半身（楔形长鼻 + 轮拱）
    const lower = new THREE.Mesh(lowerGeo([
      [-1.03, 0.07], [-1.03, 0.42],
      [0.45, 0.43],
      [0.95, 0.37],
      [1.03, 0.32],
      [1.03, 0.07],
    ], [WHEEL_Z, -WHEEL_Z], 0.175, 0.78, 0.015), white);
    lower.castShadow = true;
    car.add(lower); App.outline(lower, 0.016);

    // 座舱（溜背斜面拉到车尾）
    const cabin = new THREE.Mesh(cabinGeo([
      [0.45, 0.40], [0.0, 0.61],
      [-0.4, 0.61],
      [-1.0, 0.42],
    ], 0.68, 0.015), white);
    cabin.castShadow = true;
    car.add(cabin); App.outline(cabin, 0.014);

    // 玻璃
    car.add(slopePanel(0.0, 0.61, 0.45, 0.40, 0.62, glassMat, 0.024));
    car.add(slopePanel(-0.4, 0.61, -0.98, 0.40, 0.6, glassMat, 0.024));
    const glassBand = [[0.42, 0.43], [0.02, 0.595], [-0.36, 0.595], [-0.9, 0.43]];
    car.add(sideGlass(glassBand, 0.368, glassMat));
    car.add(sideGlass(glassBand, -0.368, glassMat));

    // 侧裙深灰
    bx(car, 0.815, 0.045, 1.9, App.toon('#42474f'), 0, 0.11, 0, 0);
    // 闭合跳灯盖板
    const coverMat = App.toon('#3a3f49');
    const c1 = bx(car, 0.22, 0.015, 0.26, coverMat, 0.26, 0.402, 0.7, 0.006);
    const c2 = bx(car, 0.22, 0.015, 0.26, coverMat, -0.26, 0.402, 0.7, 0.006);
    c1.rotation.x = -0.13; c2.rotation.x = -0.13;
    // 前转向灯带
    bx(car, 0.7, 0.05, 0.03, App.glow('#d88a2e', 0.75), 0, 0.24, 1.045, 0);
    // 全宽尾灯
    bx(car, 0.74, 0.08, 0.03, App.glow('#a83232', 0.85), 0, 0.33, -1.045, 0.006);
    // 尾翼
    bx(car, 0.04, 0.05, 0.05, trimMat, 0.24, 0.45, -0.96, 0);
    bx(car, 0.04, 0.05, 0.05, trimMat, -0.24, 0.45, -0.96, 0);
    bx(car, 0.58, 0.02, 0.09, trimMat, 0, 0.48, -0.97, 0.008);
    // 后视镜
    bx(car, 0.04, 0.05, 0.08, trimMat, 0.44, 0.48, 0.12, 0.008);
    bx(car, 0.04, 0.05, 0.08, trimMat, -0.44, 0.48, 0.12, 0.008);
    // 排气
    const ex = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 0.12, 8), rimMat2);
    ex.rotation.x = Math.PI / 2;
    ex.position.set(-0.25, 0.11, -1.05);
    car.add(ex);

    car.add(mkWheel(0.36, WHEEL_Z, WHEEL_R, 0.1, rimMat2));
    car.add(mkWheel(-0.36, WHEEL_Z, WHEEL_R, 0.1, rimMat2));
    car.add(mkWheel(0.36, -WHEEL_Z, WHEEL_R, 0.1, rimMat2));
    car.add(mkWheel(-0.36, -WHEEL_Z, WHEEL_R, 0.1, rimMat2));

    carShadow(car, 1.3, 2.55);
    return car;
  };

  App.buildCars = function (root) {
    // 一前一后停在店前路缘（朝路口 +x）：AE86 在前，FC 在后，留出车距
    const ae86 = App.buildAE86();
    ae86.position.set(-1.1, 0.03, 1.05);
    ae86.rotation.y = Math.PI / 2;
    root.add(ae86);
    const fc = App.buildFC();
    fc.position.set(-3.6, 0.03, 1.05);
    fc.rotation.y = Math.PI / 2;
    root.add(fc);
    return { ae86, fc };
  };

})(window.App);
