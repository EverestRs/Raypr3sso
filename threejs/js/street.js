/* =========================================================
 * street.js — 展示底座 + 马路 + 街道道具
 * 底座：±4.9 圆角方形；路网：前沿横路 + 右侧纵路（丁字路口）
 * 便利店占西北角：x[-3.9,2.1] z[-3.3,-0.3]
 * ========================================================= */
'use strict';
(function (App) {

  App.buildStreet = function (root) {
    const g = new THREE.Group();
    root.add(g);
    const refs = { puddleSpots: [] };

    const slab = (w, h, d, mat, x, y, z, receive, cast) => {
      const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), mat);
      m.position.set(x, y, z);
      m.receiveShadow = receive !== false;
      m.castShadow = !!cast;
      g.add(m);
      return m;
    };

    /* ================= 展示底座 ================= */
    const rimMat = App.toon('#222941');
    const pedMat = App.toon('#131826');
    const plate = new THREE.Mesh(App.roundBox(9.8, 0.16, 9.8, 0.28, 3), rimMat);
    plate.position.set(0, -0.08, 0);
    plate.receiveShadow = true;
    g.add(plate);
    App.outline(plate, 0.03);
    const pedestal = new THREE.Mesh(App.roundBox(9.3, 0.5, 9.3, 0.3, 3), pedMat);
    pedestal.position.set(0, -0.41, 0);
    g.add(pedestal);
    App.outline(pedestal, 0.03);
    // 底座铭牌
    const plateTex = App.canvasTex(512, 128, (ctx, w, h) => {
      ctx.fillStyle = '#151a28'; ctx.fillRect(0, 0, w, h);
      ctx.strokeStyle = '#c8a860'; ctx.lineWidth = 4; ctx.strokeRect(10, 10, w - 20, h - 20);
      ctx.fillStyle = '#e8cc88'; ctx.textAlign = 'center';
      ctx.font = `900 52px ${App.FONT}`;
      ctx.fillText('雨の夜 ・ よるマート前', w / 2, 82);
    });
    const nameplate = new THREE.Mesh(new THREE.BoxGeometry(1.9, 0.4, 0.06),
      new THREE.MeshBasicMaterial({ map: plateTex }));
    nameplate.position.set(0, -0.42, 4.56);
    g.add(nameplate);

    /* ================= 路面 ================= */
    const asphaltMat = App.wet('#1a1f2e', { shininess: 75, specular: new THREE.Color('#3c4c6a') });
    const walkMat = App.wet('#3d4356', { shininess: 14, specular: new THREE.Color('#2e3547') });
    const walkMat2 = App.wet('#454b5e', { shininess: 14, specular: new THREE.Color('#2e3547') });
    const curbMat = App.toon('#6d7686');

    // 前横路 x[-4.75,2.95] z[0.6,3.3]
    slab(7.7, 0.03, 2.7, asphaltMat, -0.9, 0.015, 1.95);
    // 右纵路 x[2.95,4.75] z[-4.75,4.75]
    slab(1.8, 0.03, 9.5, asphaltMat, 3.85, 0.015, 0);
    // 人行道
    slab(7.7, 0.06, 0.9, walkMat, -0.9, 0.03, 0.15);        // 店前
    slab(0.85, 0.06, 4.45, walkMat, 2.525, 0.03, -2.525);   // 店右侧
    slab(9.5, 0.06, 1.45, walkMat2, 0, 0.03, 4.025);        // 路对面
    // 路缘石
    slab(7.7, 0.075, 0.12, curbMat, -0.9, 0.0375, 0.6, true, false);
    slab(9.5, 0.075, 0.12, curbMat, 0, 0.0375, 3.3, true, false);
    slab(0.12, 0.075, 5.35, curbMat, 2.95, 0.0375, -2.075, true, false);
    slab(0.12, 0.075, 9.5, curbMat, 4.69, 0.0375, 0, true, false);
    // 排水沟（贴路缘的暗槽）
    const gutterMat = App.toon('#0d1119');
    slab(7.4, 0.012, 0.1, gutterMat, -0.9, 0.04, 0.75, false, false);
    slab(0.1, 0.012, 5.0, gutterMat, 3.1, 0.04, -2.2, false, false);
    // 排水格栅
    const grateMat = App.toon('#39404f');
    for (const [gx, gz] of [[-2.6, 0.75], [-1.0, 0.75], [1.4, 0.75], [3.1, -0.6]]) {
      slab(0.55, 0.016, 0.12, grateMat, gx, 0.043, gz, false, false);
    }

    // 点字导盲砖
    const tenjiMat = App.toon('#c8a83a');
    slab(5.5, 0.008, 0.24, tenjiMat, -0.85, 0.071, 0.4, false, false);
    slab(0.24, 0.008, 3.2, tenjiMat, 2.68, 0.071, -2.6, false, false);

    /* ---------- 交通标线 ---------- */
    const paintMat = App.wet('#d8d6cc', { shininess: 100, specular: new THREE.Color('#8fa4c8') });
    // 斑马线（反光）
    for (let i = 0; i < 5; i++) {
      slab(1.5, 0.006, 0.2, paintMat, 0.75, 0.042, 0.98 + i * 0.5, false, false);
    }
    // 车道虚线
    for (let x = -4.4; x < 2.7; x += 1.0) slab(0.45, 0.005, 0.07, paintMat, x, 0.042, 1.95, false, false);
    for (let z = -4.4; z < 3.0; z += 1.0) slab(0.07, 0.005, 0.45, paintMat, 3.85, 0.042, z, false, false);
    // 停车位（右侧路边的投币停车框）
    slab(1.3, 0.005, 0.07, paintMat, 3.7, 0.042, -1.9, false, false);
    slab(1.3, 0.005, 0.07, paintMat, 3.7, 0.042, 0.2, false, false);
    slab(0.07, 0.005, 2.1, paintMat, 3.08, 0.042, -0.85, false, false);
    slab(0.07, 0.005, 2.1, paintMat, 4.32, 0.042, -0.85, false, false);
    // 停止线
    slab(1.7, 0.006, 0.12, paintMat, 3.85, 0.042, 2.95, false, false);

    /* ---------- 积水洼 ---------- */
    const puddleMat = App.wet('#111722', { shininess: 170, specular: new THREE.Color('#93aad4') });
    const mkPuddle = (x, y, z, sx, sz, rot, ripple) => {
      const p = new THREE.Mesh(new THREE.CircleGeometry(1, 26), puddleMat);
      p.rotation.x = -Math.PI / 2;
      p.rotation.z = rot;
      p.scale.set(sx, sz, 1);
      p.position.set(x, y, z);
      p.renderOrder = 2;
      g.add(p);
      if (ripple) refs.puddleSpots.push(new THREE.Vector3(x, y + 0.005, z));
      return p;
    };
    mkPuddle(-1.3, 0.045, 1.5, 1.35, 0.55, 0.3, true);
    mkPuddle(1.7, 0.046, 2.45, 0.95, 0.45, -0.5, true);
    mkPuddle(-3.5, 0.045, 2.35, 0.8, 0.38, 0.9, true);
    mkPuddle(3.55, 0.046, -0.9, 0.7, 1.1, 0.2, true);
    mkPuddle(2.45, 0.072, -1.6, 0.42, 0.8, 0.5, true);   // 人行道上小水洼
    mkPuddle(0.3, 0.072, 3.9, 0.9, 0.4, -0.2, true);     // 对面人行道

    /* ---------- 湿地灯光池 ---------- */
    const addPool = (tex, w, d, x, y, z, op, rot) => {
      const d1 = App.decal(w, d, tex, { opacity: op, y });
      d1.position.set(x, y, z);
      if (rot) d1.rotation.z = rot;
      g.add(d1);
      return d1;
    };
    const warmStreak = App.streakTex('rgba(255,190,120,0.85)');
    addPool(warmStreak, 5.4, 0.95, -0.85, 0.074, 0.18, 0.42);            // 店前人行道暖光
    addPool(warmStreak, 2.2, 1.5, 0.55, 0.047, 1.6, 0.28);              // 门前路面
    addPool(App.radialTex('rgba(255,214,150,0.9)', 'rgba(255,214,150,0)'),
      2.0, 2.0, -3.3, 0.047, 1.1, 0.3);                                  // 路灯池
    addPool(App.streakTex('rgba(255,120,120,0.8)'), 0.8, 1.6, 3.35, 0.074, -1.7, 0.24);// 贩卖机
    // 红绿灯灯光池（颜色随信号变化）
    const trafficPoolMat = new THREE.MeshBasicMaterial({
      map: App.radialTex('rgba(255,255,255,0.9)', 'rgba(255,255,255,0)'),
      transparent: true, opacity: 0.3, blending: THREE.AdditiveBlending,
      depthWrite: false,
    });
    const trafficPool = new THREE.Mesh(new THREE.PlaneGeometry(1.6, 1.6), trafficPoolMat);
    trafficPool.rotation.x = -Math.PI / 2;
    trafficPool.rotation.z = 0.6;
    trafficPool.position.set(3.9, 0.047, 3.1);
    trafficPool.renderOrder = 5;
    g.add(trafficPool);
    refs.trafficPoolMat = trafficPoolMat;

    /* ================= 街道道具 ================= */
    const steelMat = App.toon('#7c8698');
    const darkSteel = App.toon('#3a4254');
    const whiteMat = App.toon('#b6bdc9');

    /* ---------- 路灯 ---------- */
    const lampGrp = new THREE.Group();
    const lampPole = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.07, 3.0, 10), steelMat);
    lampPole.position.y = 1.5;
    lampPole.castShadow = true;
    lampGrp.add(lampPole); App.outline(lampPole, 0.018);
    const lampBase = new THREE.Mesh(new THREE.CylinderGeometry(0.12, 0.15, 0.18, 10), darkSteel);
    lampBase.position.y = 0.09;
    lampGrp.add(lampBase); App.outline(lampBase, 0.015);
    const arm = new THREE.Mesh(new THREE.CylinderGeometry(0.035, 0.04, 0.72, 8), steelMat);
    arm.position.set(0, 3.14, 0.3);
    arm.quaternion.setFromUnitVectors(
      new THREE.Vector3(0, 1, 0),
      new THREE.Vector3(0, 0.3, 0.62).normalize()
    );
    lampGrp.add(arm);
    const headMat = App.toon('#39404f');
    const head = new THREE.Mesh(App.roundBox(0.26, 0.12, 0.62, 0.03), headMat);
    head.position.set(0, 3.32, 0.68);
    lampGrp.add(head); App.outline(head, 0.018);
    const headGlow = new THREE.Mesh(new THREE.PlaneGeometry(0.2, 0.5),
      new THREE.MeshBasicMaterial({ color: '#ffe6b8' }));
    headGlow.rotation.x = Math.PI / 2;
    headGlow.position.set(0, 3.24, 0.68);
    lampGrp.add(headGlow);
    const lampLight = new THREE.PointLight('#ffd9a0', 0.85, 6.5, 2);
    lampLight.position.set(0, 3.05, 0.68);
    lampGrp.add(lampLight);
    refs.lampLight = lampLight;
    lampGrp.position.set(-3.3, 0.06, 0.15);
    g.add(lampGrp);

    /* ---------- 电线杆 + 电线 ---------- */
    const poleGrp = new THREE.Group();
    const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.09, 0.11, 3.8, 10), App.toon('#83838d'));
    pole.position.y = 1.9;
    pole.castShadow = true;
    poleGrp.add(pole); App.outline(pole, 0.02);
    const crossarm = new THREE.Mesh(new THREE.BoxGeometry(1.1, 0.07, 0.07), App.toon('#6a6a72'));
    crossarm.position.y = 3.45;
    poleGrp.add(crossarm); App.outline(crossarm, 0.015);
    const crossarm2 = new THREE.Mesh(new THREE.BoxGeometry(0.7, 0.06, 0.06), App.toon('#6a6a72'));
    crossarm2.position.y = 3.0;
    poleGrp.add(crossarm2);
    // 变压器
    const trans = new THREE.Mesh(new THREE.CylinderGeometry(0.2, 0.2, 0.5, 12), App.toon('#75757e'));
    trans.position.set(0.35, 2.6, 0);
    trans.castShadow = true;
    poleGrp.add(trans); App.outline(trans, 0.02);
    const trans2 = trans.clone(); trans2.position.set(0.35, 2.0, 0); poleGrp.add(trans2);
    poleGrp.position.set(2.5, 0.06, -4.3);
    g.add(poleGrp);

    const wireMat = App.toon('#141822');
    const wire = (a, b, sag) => {
      const mid = a.clone().add(b).multiplyScalar(0.5);
      mid.y -= sag;
      const curve = new THREE.QuadraticBezierCurve3(a, mid, b);
      const m = new THREE.Mesh(new THREE.TubeGeometry(curve, 18, 0.013, 5), wireMat);
      g.add(m);
      return m;
    };
    const pTop = new THREE.Vector3(2.5, 3.62, -4.3);
    wire(pTop, new THREE.Vector3(2.14, 2.85, -2.4), 0.25);        // → 店右墙
    wire(pTop, new THREE.Vector3(0.4, 2.5, -4.55), 0.2);          // → 后侧邻楼
    wire(pTop, new THREE.Vector3(4.35, 2.92, 3.85), 0.55);        // → 路口红绿灯杆

    /* ---------- 红绿灯 ---------- */
    const tlGrp = new THREE.Group();
    const tlPole = new THREE.Mesh(new THREE.CylinderGeometry(0.055, 0.075, 2.9, 10), darkSteel);
    tlPole.position.y = 1.45;
    tlPole.castShadow = true;
    tlGrp.add(tlPole); App.outline(tlPole, 0.018);
    const tlArm = new THREE.Mesh(new THREE.CylinderGeometry(0.04, 0.04, 0.95, 8), darkSteel);
    tlArm.rotation.z = Math.PI / 2;
    tlArm.position.set(-0.45, 2.82, 0);
    tlGrp.add(tlArm);
    const tlBox = new THREE.Mesh(App.roundBox(0.24, 0.72, 0.2, 0.03), App.toon('#232833'));
    tlBox.position.set(-0.85, 2.6, 0);
    tlGrp.add(tlBox); App.outline(tlBox, 0.018);
    const tlMats = {};
    [['r', '#ff5a4d', 0.22], ['y', '#ffc84d', 0], ['g', '#54e07c', -0.22]].forEach(([k, c, y]) => {
      const m = new THREE.MeshBasicMaterial({ color: '#141821' });
      const lamp = new THREE.Mesh(new THREE.CircleGeometry(0.085, 16), m);
      lamp.position.set(-0.85, 2.6 + y, 0.13);
      tlGrp.add(lamp);
      tlMats[k] = m;
    });
    tlGrp.position.set(4.35, 0.06, 3.85);
    tlGrp.rotation.y = 0;   // 灯面朝 +z（观众方向）
    g.add(tlGrp);
    refs.trafficMats = tlMats;

    /* ---------- 凸面镜 ---------- */
    const cmGrp = new THREE.Group();
    const cmPole = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.04, 1.7, 8), App.toon('#d8783a'));
    cmPole.position.y = 0.85;
    cmGrp.add(cmPole); App.outline(cmPole, 0.012);
    const cmBack = new THREE.Mesh(new THREE.SphereGeometry(0.22, 18, 12, 0, Math.PI * 2, 0, Math.PI / 2), App.toon('#e8783a'));
    cmBack.rotation.x = Math.PI / 2;
    cmBack.position.y = 1.78;
    cmGrp.add(cmBack); App.outline(cmBack, 0.014);
    const cmGlass = new THREE.Mesh(new THREE.CircleGeometry(0.2, 18),
      new THREE.MeshPhongMaterial({ color: '#aec6e0', shininess: 200, specular: new THREE.Color('#ffffff') }));
    cmGlass.position.set(0, 1.78, 0.235);
    cmGlass.rotation.x = -0.15;
    cmGrp.add(cmGlass);
    cmGrp.position.set(2.5, 0.06, 0.42);
    cmGrp.rotation.y = Math.PI / 4;
    g.add(cmGrp);

    /* ---------- 自动贩卖机 ×2（贴店右墙） ---------- */
    const vending = (z, hot) => {
      const grp = new THREE.Group();
      const body = new THREE.Mesh(App.roundBox(0.75, 1.75, 0.62, 0.03),
        App.toon(hot ? '#e8e2d4' : '#d84848'));
      body.position.y = 0.875;
      body.castShadow = true;
      grp.add(body); App.outline(body, 0.02);
      const face = new THREE.Mesh(new THREE.PlaneGeometry(0.6, 1.5),
        new THREE.MeshBasicMaterial({ map: App.vendingTex(hot) }));
      face.rotation.y = Math.PI / 2;
      face.position.set(0.415, 0.95, 0);
      grp.add(face);
      // 顶部灯带
      const strip = new THREE.Mesh(new THREE.BoxGeometry(0.06, 0.1, 0.5), App.glow(hot ? '#ffdf9f' : '#ff9a9a', 1.2));
      strip.position.set(0.40, 1.72, 0);
      grp.add(strip);
      const vmLight = new THREE.PointLight(hot ? '#ffcf8f' : '#ffb0a8', 0.4, 2.2, 2);
      vmLight.position.set(0.7, 1.2, 0);
      grp.add(vmLight);
      grp.position.set(2.48, 0.06, z);
      return grp;
    };
    g.add(vending(-1.35, false));
    g.add(vending(-2.1, true));

    /* ---------- 公告栏 ---------- */
    const bbGrp = new THREE.Group();
    const bbCase = new THREE.Mesh(App.roundBox(0.9, 1.1, 0.1, 0.02), App.toon('#3a4254'));
    bbCase.position.y = 1.05;
    bbCase.castShadow = true;
    bbGrp.add(bbCase); App.outline(bbCase, 0.018);
    const bbFace = new THREE.Mesh(new THREE.PlaneGeometry(0.8, 1.0),
      new THREE.MeshBasicMaterial({ map: App.bulletinTex() }));
    bbFace.rotation.y = Math.PI / 2;
    bbFace.position.set(0.09, 1.05, 0);
    bbGrp.add(bbFace);
    for (const [lx, lz] of [[-0.32, 0], [0.32, 0]]) {
      const leg = new THREE.Mesh(new THREE.BoxGeometry(0.07, 0.5, 0.07), darkSteel);
      leg.position.set(lx, 0.25, lz);
      bbGrp.add(leg); App.outline(leg, 0.012);
    }
    bbGrp.position.set(2.55, 0.06, -2.85);
    bbGrp.rotation.y = Math.PI / 2 + 0.15;
    g.add(bbGrp);

    /* ---------- 自行车 ---------- */
    const bikeGrp = new THREE.Group();
    const bikeMat = App.toon('#7fc8b8');
    const tireMat = App.toon('#2a2e38');
    const wheel = (x, z) => {
      const w = new THREE.Mesh(new THREE.TorusGeometry(0.24, 0.028, 8, 20), tireMat);
      w.position.set(x, 0.27, z);
      w.rotation.y = Math.PI / 2;
      bikeGrp.add(w); App.outline(w, 0.012);
      const hub = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 0.06, 8), steelMat);
      hub.rotation.x = Math.PI / 2;
      hub.position.set(x, 0.27, z);
      bikeGrp.add(hub);
      // 三根辐条
      for (let i = 0; i < 3; i++) {
        const sp = new THREE.Mesh(new THREE.BoxGeometry(0.44, 0.008, 0.008), steelMat);
        sp.position.copy(hub.position);
        sp.rotation.x = Math.PI / 2;
        sp.rotation.y = 0;
        sp.rotation.z = i * Math.PI / 3;
        bikeGrp.add(sp);
      }
    };
    wheel(-0.38, 0); wheel(0.38, 0);
    // 车架
    const tube = (a, b, r) => {
      const dir = new THREE.Vector3().subVectors(b, a);
      const t = new THREE.Mesh(new THREE.CylinderGeometry(r, r, dir.length(), 7), bikeMat);
      t.position.copy(a).add(b).multiplyScalar(0.5);
      t.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir.clone().normalize());
      bikeGrp.add(t);
      return t;
    };
    const V = (x, y, z) => new THREE.Vector3(x, y, z);
    tube(V(-0.38, 0.27, 0), V(-0.1, 0.62, 0), 0.022);   // 后上
    tube(V(-0.38, 0.27, 0), V(-0.02, 0.3, 0), 0.022);   // 后下
    tube(V(-0.1, 0.62, 0), V(0.28, 0.6, 0), 0.022);     // 上管
    tube(V(-0.02, 0.3, 0), V(0.28, 0.6, 0), 0.022);     // 下管
    tube(V(0.28, 0.6, 0), V(0.38, 0.82, 0), 0.02);      // 头管
    tube(V(0.38, 0.27, 0), V(0.38, 0.82, 0), 0.02);     // 前叉
    // 车座 / 把手
    const seat = new THREE.Mesh(App.roundBox(0.2, 0.05, 0.1, 0.015), App.toon('#2a2e38'));
    seat.position.set(-0.14, 0.72, 0);
    bikeGrp.add(seat); App.outline(seat, 0.01);
    tube(V(-0.1, 0.62, 0), V(-0.14, 0.7, 0), 0.018);
    const hb = new THREE.Mesh(new THREE.CylinderGeometry(0.015, 0.015, 0.4, 6), steelMat);
    hb.rotation.x = Math.PI / 2;
    hb.position.set(0.38, 0.86, 0);
    bikeGrp.add(hb);
    tube(V(0.38, 0.82, 0), V(0.38, 0.86, 0), 0.018);
    // 前筐
    const basket = new THREE.Mesh(App.roundBox(0.24, 0.16, 0.3, 0.015), App.toon('#4a5160'));
    basket.position.set(0.42, 0.72, 0);
    bikeGrp.add(basket); App.outline(basket, 0.012);
    // 挡泥板
    const fender = new THREE.Mesh(new THREE.TorusGeometry(0.26, 0.02, 6, 12, Math.PI * 0.7), bikeMat);
    fender.position.set(-0.38, 0.27, 0);
    fender.rotation.y = Math.PI / 2;
    fender.rotation.x = Math.PI / 2 - Math.PI * 0.15;
    bikeGrp.add(fender);
    bikeGrp.rotation.y = 0.12;
    bikeGrp.rotation.z = 0.05;
    bikeGrp.position.set(1.5, 0.09, 0.18);
    g.add(bikeGrp);

    /* ---------- 伞架 + 垃圾桶 + 立牌 ---------- */
    // 伞架
    const ubGrp = new THREE.Group();
    const ubCan = new THREE.Mesh(new THREE.CylinderGeometry(0.13, 0.11, 0.34, 12, 1, true), darkSteel);
    ubCan.position.y = 0.17;
    ubCan.material.side = THREE.DoubleSide;
    ubGrp.add(ubCan); App.outline(ubCan, 0.012);
    const ubBottom = new THREE.Mesh(new THREE.CircleGeometry(0.11, 12), darkSteel);
    ubBottom.rotation.x = -Math.PI / 2;
    ubBottom.position.y = 0.012;
    ubGrp.add(ubBottom);
    const umb1 = new THREE.Mesh(new THREE.CylinderGeometry(0.018, 0.024, 0.55, 8), App.toon('#c85050'));
    umb1.position.set(0.04, 0.32, 0.02);
    umb1.rotation.z = 0.14;
    ubGrp.add(umb1); App.outline(umb1, 0.008);
    const umb2 = new THREE.Mesh(new THREE.CylinderGeometry(0.018, 0.024, 0.55, 8), App.toon('#3a5a8f'));
    umb2.position.set(-0.05, 0.31, -0.03);
    umb2.rotation.z = -0.12;
    ubGrp.add(umb2); App.outline(umb2, 0.008);
    ubGrp.position.set(1.12, 0.06, -0.08);
    g.add(ubGrp);

    // 垃圾桶
    const binGrp = new THREE.Group();
    const binBody = new THREE.Mesh(App.roundBox(0.32, 0.72, 0.32, 0.02), App.toon('#5a6272'));
    binBody.position.y = 0.36;
    binBody.castShadow = true;
    binGrp.add(binBody); App.outline(binBody, 0.016);
    const binTop = new THREE.Mesh(new THREE.BoxGeometry(0.34, 0.05, 0.34), App.toon('#39404f'));
    binTop.position.y = 0.74;
    binGrp.add(binTop);
    const binSlot = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.02, 0.05), App.toon('#10141c'));
    binSlot.position.y = 0.77;
    binGrp.add(binSlot);
    binGrp.position.set(-0.4, 0.06, 0.16);
    g.add(binGrp);

    // A 字立牌
    const sbTex = App.canvasTex(256, 320, (ctx, w, h) => {
      ctx.fillStyle = '#f5f0e0'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = '#d84848'; ctx.fillRect(0, 0, w, 70);
      ctx.fillStyle = '#fff'; ctx.font = `900 44px ${App.FONT}`; ctx.textAlign = 'center';
      ctx.fillText('本日の', w / 2, 52);
      ctx.fillStyle = '#2c3a55'; ctx.font = `900 56px ${App.FONT}`;
      ctx.fillText('おすすめ', w / 2, 150);
      ctx.fillStyle = '#e88a3a';
      ctx.beginPath(); ctx.arc(w / 2, 232, 52, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#fff'; ctx.font = `900 40px ${App.FONT}`;
      ctx.fillText('汁', w / 2, 248);
    });
    const sbGrp = new THREE.Group();
    const sbMat = new THREE.MeshToonMaterial({ map: sbTex, gradientMap: App.gradientMap, side: THREE.DoubleSide });
    const sbA = new THREE.Mesh(new THREE.PlaneGeometry(0.5, 0.62), sbMat);
    sbA.rotation.x = -0.16;
    sbA.position.set(0, 0.32, 0.03);
    const sbB = sbA.clone();
    sbB.rotation.x = 0.16;
    sbB.position.z = -0.03;
    sbGrp.add(sbA, sbB);
    App.outline(sbA, 0.012); App.outline(sbB, 0.012);
    sbGrp.rotation.y = -0.35;
    sbGrp.position.set(-1.05, 0.06, 0.28);
    g.add(sbGrp);

    /* ---------- 路名牌 ---------- */
    const snGrp = new THREE.Group();
    const snPole = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.04, 2.3, 8), steelMat);
    snPole.position.y = 1.15;
    snGrp.add(snPole); App.outline(snPole, 0.014);
    const snTex = App.streetSignTex();
    const snMat = new THREE.MeshToonMaterial({ map: snTex, gradientMap: App.gradientMap, side: THREE.DoubleSide });
    const snPlate = new THREE.Mesh(new THREE.BoxGeometry(0.95, 0.3, 0.03), App.toon('#2f6f4f'));
    snPlate.position.y = 2.1;
    snGrp.add(snPlate); App.outline(snPlate, 0.014);
    const snFace = new THREE.Mesh(new THREE.PlaneGeometry(0.9, 0.25), new THREE.MeshBasicMaterial({ map: snTex }));
    snFace.position.set(0, 2.1, 0.04);
    snGrp.add(snFace);
    const snFace2 = snFace.clone();
    snFace2.rotation.y = Math.PI;
    snFace2.position.z = -0.04;
    snGrp.add(snFace2);
    snGrp.position.set(-1.3, 0.06, 4.3);
    g.add(snGrp);

    /* ---------- 护栏（对面路缘） ---------- */
    const railGrp = new THREE.Group();
    const railMat = whiteMat;
    const railBeam = new THREE.Mesh(new THREE.BoxGeometry(5.2, 0.14, 0.05), railMat);
    railBeam.position.set(0, 0.5, 0);
    railBeam.castShadow = true;
    railGrp.add(railBeam); App.outline(railBeam, 0.014);
    const railBeam2 = new THREE.Mesh(new THREE.BoxGeometry(5.2, 0.08, 0.04), railMat);
    railBeam2.position.set(0, 0.26, 0);
    railGrp.add(railBeam2);
    for (let x = -2.4; x <= 2.4; x += 0.8) {
      const post = new THREE.Mesh(new THREE.BoxGeometry(0.07, 0.55, 0.07), railMat);
      post.position.set(x, 0.275, 0);
      railGrp.add(post); App.outline(post, 0.01);
    }
    railGrp.position.set(-1.1, 0.06, 4.62);
    g.add(railGrp);

    /* ---------- 花坛 / 树 ---------- */
    const planterMat = App.toon('#655e50');
    const leafMat = App.toon('#31593c');
    const leafMat2 = App.toon('#3e6a4a');
    const planter = (x, z, tree) => {
      const grp = new THREE.Group();
      const box = new THREE.Mesh(App.roundBox(0.55, 0.4, 0.55, 0.02), planterMat);
      box.position.y = 0.2;
      box.castShadow = true;
      grp.add(box); App.outline(box, 0.016);
      const soil = new THREE.Mesh(new THREE.BoxGeometry(0.47, 0.03, 0.47), App.toon('#3a3228'));
      soil.position.y = 0.41;
      grp.add(soil);
      if (tree) {
        const trunk = new THREE.Mesh(new THREE.CylinderGeometry(0.045, 0.06, 1.0, 8), App.toon('#5a4636'));
        trunk.position.y = 0.9;
        trunk.castShadow = true;
        grp.add(trunk); App.outline(trunk, 0.014);
        const b1 = new THREE.Mesh(new THREE.IcosahedronGeometry(0.42, 1), leafMat);
        b1.position.y = 1.65;
        b1.scale.y = 0.85;
        b1.castShadow = true;
        grp.add(b1); App.outline(b1, 0.02);
        const b2 = new THREE.Mesh(new THREE.IcosahedronGeometry(0.3, 1), leafMat2);
        b2.position.set(0.18, 1.35, 0.1);
        grp.add(b2); App.outline(b2, 0.016);
      } else {
        const bush = new THREE.Mesh(new THREE.IcosahedronGeometry(0.24, 1), leafMat);
        bush.position.y = 0.5;
        bush.scale.y = 0.8;
        bush.castShadow = true;
        grp.add(bush); App.outline(bush, 0.016);
        const bush2 = new THREE.Mesh(new THREE.IcosahedronGeometry(0.17, 1), leafMat2);
        bush2.position.set(0.14, 0.44, 0.1);
        grp.add(bush2); App.outline(bush2, 0.012);
      }
      grp.position.set(x, 0.06, z);
      g.add(grp);
      return grp;
    };
    planter(-2.6, 0.18, false);
    planter(-4.05, 3.9, true);
    planter(2.35, 4.15, false);

    /* ---------- 邻楼 + 后巷 ---------- */
    const nbMat = App.toon('#4e4960');
    const nb = slab(4.3, 2.3, 0.9, nbMat, -1.25, 0.03 + 1.15, -4.3, true, true);
    App.outline(nb, 0.025);
    // 邻楼窗（部分亮灯）
    const winLit = new THREE.MeshBasicMaterial({ color: '#ffcf8f' });
    const winDark = new THREE.MeshBasicMaterial({ color: '#252c3e' });
    for (let i = 0; i < 5; i++) {
      for (let j = 0; j < 2; j++) {
        const lit = (i + j) % 3 === 0;
        const win = new THREE.Mesh(new THREE.PlaneGeometry(0.42, 0.55), lit ? winLit : winDark);
        win.position.set(-3.0 + i * 0.9, 0.75 + j * 1.05, -3.84);
        g.add(win);
        const fr = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.63, 0.03), App.toon('#4a4658'));
        fr.position.set(-3.0 + i * 0.9, 0.75 + j * 1.05, -3.86);
        g.add(fr);
      }
    }
    // 邻楼屋顶压顶 + 通风口
    slab(4.44, 0.1, 1.04, App.toon('#3e3a4e'), -1.25, 2.33, -4.3, false, false);
    slab(0.3, 0.18, 0.3, App.toon('#6a6478'), 0.2, 2.43, -4.2, false, false);
    slab(0.22, 0.12, 0.22, App.toon('#6a6478'), -3.0, 2.41, -4.45, false, false);
    // 巷口门口灯
    const aLampHead = new THREE.Mesh(new THREE.BoxGeometry(0.12, 0.08, 0.12), darkSteel);
    aLampHead.position.set(1.05, 1.75, -3.42);
    g.add(aLampHead);
    const aLampGlow = new THREE.Mesh(new THREE.PlaneGeometry(0.09, 0.05), App.glow('#ffe2a8', 1.1));
    aLampGlow.position.set(1.05, 1.7, -3.4);
    g.add(aLampGlow);
    const aLampLight = new THREE.PointLight('#ffd9a0', 0.3, 1.8, 2);
    aLampLight.position.set(1.05, 1.6, -3.6);
    g.add(aLampLight);

    // 邻楼屋顶水塔
    const wt = new THREE.Mesh(new THREE.CylinderGeometry(0.28, 0.28, 0.5, 12), App.toon('#6a6478'));
    wt.position.set(-2.2, 2.5, -4.3);
    wt.castShadow = true;
    g.add(wt); App.outline(wt, 0.02);
    const wtLegs = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 0.2, 8), darkSteel);
    wtLegs.position.set(-2.2, 2.17, -4.3);
    g.add(wtLegs);

    // 后巷栅栏 + 门
    const fenceMat = new THREE.MeshBasicMaterial({ color: '#4a7a5a', transparent: true, opacity: 0.35, side: THREE.DoubleSide });
    const fenceSeg = (x0, x1) => {
      const len = x1 - x0;
      const mesh = new THREE.Mesh(new THREE.PlaneGeometry(len, 0.95), fenceMat);
      mesh.position.set((x0 + x1) / 2, 0.06 + 0.475, -3.5);
      g.add(mesh);
      const bar1 = new THREE.Mesh(new THREE.BoxGeometry(len, 0.04, 0.04), steelMat);
      bar1.position.set((x0 + x1) / 2, 0.06 + 0.9, -3.5);
      g.add(bar1);
      const bar2 = bar1.clone(); bar2.position.y = 0.06 + 0.08; g.add(bar2);
      for (let x = x0; x <= x1 + 0.01; x += 0.55) {
        const post = new THREE.Mesh(new THREE.BoxGeometry(0.05, 1.0, 0.05), steelMat);
        post.position.set(x, 0.06 + 0.5, -3.5);
        g.add(post);
      }
    };
    fenceSeg(-4.3, 0.7);   // 门洞留在 0.7~1.4
    fenceSeg(1.4, 2.0);
    // 巷内垃圾桶群
    const dumb1 = new THREE.Mesh(App.roundBox(0.6, 0.7, 0.38, 0.02), App.toon('#31593c'));
    dumb1.position.set(-2.0, 0.06 + 0.35, -3.62);
    dumb1.castShadow = true;
    g.add(dumb1); App.outline(dumb1, 0.016);
    const dumb2 = new THREE.Mesh(App.roundBox(0.6, 0.7, 0.38, 0.02), App.toon('#3a5a8f'));
    dumb2.position.set(-1.3, 0.06 + 0.35, -3.62);
    dumb2.castShadow = true;
    g.add(dumb2); App.outline(dumb2, 0.016);
    const dumbLid1 = new THREE.Mesh(new THREE.BoxGeometry(0.64, 0.06, 0.42), App.toon('#2c4a38'));
    dumbLid1.position.set(-2.0, 0.76 + 0.06, -3.62);
    g.add(dumbLid1);
    const dumbLid2 = dumbLid1.clone(); dumbLid2.position.set(-1.3, 0.82, -3.62); dumbLid2.material = App.toon('#2c3e5f');
    g.add(dumbLid2);
    // 巷口立入禁止牌
    const noEnter = new THREE.Mesh(new THREE.PlaneGeometry(0.3, 0.22),
      new THREE.MeshBasicMaterial({ map: App.canvasTex(128, 96, (ctx, w, h) => {
        ctx.fillStyle = '#f0ede4'; ctx.fillRect(0, 0, w, h);
        ctx.strokeStyle = '#c83a3a'; ctx.lineWidth = 6; ctx.strokeRect(6, 6, w - 12, h - 12);
        ctx.fillStyle = '#c83a3a'; ctx.font = `900 30px ${App.FONT}`; ctx.textAlign = 'center';
        ctx.fillText('立入禁止', w / 2, 62);
      }) }));
    noEnter.position.set(1.05, 1.0, -3.46);
    g.add(noEnter);

    /* ---------- 门垫 ---------- */
    const mat = new THREE.Mesh(new THREE.PlaneGeometry(1.15, 0.55),
      new THREE.MeshToonMaterial({ map: App.matTex(), gradientMap: App.gradientMap }));
    mat.rotation.x = -Math.PI / 2;
    mat.position.set(0.51, 0.08, 0.02);
    mat.receiveShadow = true;
    g.add(mat);

    /* ---------- 井盖 ---------- */
    for (const [mx, mz, mr] of [[0.6, 2.2, 0.24], [3.85, 1.6, 0.2], [-2.9, 1.6, 0.22]]) {
      const mh = new THREE.Mesh(new THREE.CylinderGeometry(mr, mr, 0.008, 20), App.wet('#1c222e', { shininess: 130, specular: new THREE.Color('#6a80a8') }));
      mh.position.set(mx, 0.045, mz);
      g.add(mh);
    }

    root.add(g);
    return refs;
  };

})(window.App);
