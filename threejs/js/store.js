/* =========================================================
 * store.js — 便利店建筑 + 店内陈设
 * 建筑体块：x[-3.9, 2.1]  z[-3.3, -0.3]  墙高 3.0
 * ========================================================= */
'use strict';
(function (App) {

  App.buildStore = function (root) {
    const g = new THREE.Group();
    root.add(g);

    const refs = { flicker: [], anims: {} };
    const B = (w, h, d, mat, x, y, z, ow) => {
      const m = new THREE.Mesh(App.roundBox(w, h, d, Math.min(0.02, h / 2 - 1e-3)), mat);
      m.position.set(x, y + h / 2, z);
      m.castShadow = true; m.receiveShadow = true;
      g.add(m);
      if (ow !== undefined) App.outline(m, ow);
      return m;
    };
    const P = (w, h, mat, x, y, z, ry) => { // 薄板
      const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, 0.02), mat);
      m.position.set(x, y, z);
      if (ry) m.rotation.y = ry;
      g.add(m);
      return m;
    };

    /* ---------- 外壳 ---------- */
    const wallMat = App.toon('#d9d0bb');           // 奶油外墙
    const trimMat = App.toon('#2f3a52');           // 深蓝线脚
    const steelMat = App.toon('#8b93a4');          // 金属
    const darkMat = App.toon('#232a3a');

    // 地台（店内地面）
    const floor = new THREE.Mesh(App.roundBox(5.94, 0.12, 2.94, 0.015),
      App.wet('#cfc4b0', { shininess: 70, specular: new THREE.Color('#775f3a') }));
    floor.position.set(-0.9, 0.06, -1.8);
    floor.receiveShadow = true;
    g.add(floor);

    // 后墙
    B(6.0, 3.0, 0.12, wallMat, -0.9, 0.12, -3.24, 0.02);
    // 左墙
    B(0.12, 3.0, 3.0, wallMat, -3.84, 0.12, -1.8, 0.02);
    // 右墙
    B(0.12, 3.0, 3.0, wallMat, 2.04, 0.12, -1.8, 0.02);

    // 前脸腰墙
    B(6.0, 0.55, 0.12, wallMat, -0.9, 0.12, -0.36, 0.02);
    // 前脸上部檐墙（招牌背后）
    B(6.0, 0.5, 0.1, wallMat, -0.9, 2.53, -0.36, 0.02);
    // 前脸檐口带（腰墙顶饰）
    B(6.0, 0.07, 0.16, trimMat, -0.9, 0.62, -0.36, 0.015);

    // ---- 前脸玻璃体系 ----
    const glassMat = new THREE.MeshPhongMaterial({
      color: '#8aa4c8', transparent: true, opacity: 0.13,
      shininess: 160, specular: new THREE.Color('#4e6390'), depthWrite: false,
    });
    const frameMat = App.toon('#3a4258');

    // 竖梃：左窗与门之间 / 门与右窗之间
    B(0.09, 1.95, 0.1, frameMat, -0.15, 0.69, -0.36, 0.015);
    B(0.09, 1.95, 0.1, frameMat, 1.17, 0.69, -0.36, 0.015);
    // 左侧边梃 / 右侧边梃
    B(0.08, 1.95, 0.1, frameMat, -3.5, 0.69, -0.36, 0.015);
    B(0.08, 1.95, 0.1, frameMat, 1.98, 0.69, -0.36, 0.015);
    // 门楣横梁与门下横档
    B(2.5, 0.28, 0.12, trimMat, 0.51, 2.32, -0.36, 0.015);

    // 玻璃：左大窗
    P(3.26, 1.86, glassMat, -1.88, 1.6, -0.36);
    // 玻璃：右小窗
    P(0.72, 1.86, glassMat, 1.58, 1.6, -0.36);

    // ---- 自动门（两扇滑动玻璃） ----
    const doorGlassL = P(0.66, 1.88, glassMat.clone(), 0.18, 1.5, -0.345);
    const doorGlassR = P(0.66, 1.88, glassMat.clone(), 0.84, 1.5, -0.345);
    // 门框竖条（贴在门上随门滑动）
    const dgL = App.toon('#4a5568'), dgR = App.toon('#4a5568');
    const sealL = P(0.05, 1.9, dgL, -0.13, 1.5, -0.36);
    const sealR = P(0.05, 1.9, dgR, 1.15, 1.5, -0.36);
    sealL.parent.remove(sealL); doorGlassL.add(sealL); sealL.position.set(-0.335, 0, 0.005);
    sealR.parent.remove(sealR); doorGlassR.add(sealR); sealR.position.set(0.335, 0, 0.005);
    // 门顶贴纸（营业中）
    const openTex = App.canvasTex(256, 64, (ctx, w, h) => {
      ctx.fillStyle = '#2c8f5f'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = '#fff'; ctx.font = `900 40px ${App.FONT}`; ctx.textAlign = 'center';
      ctx.fillText('営業中', w / 2, 46);
    });
    const openPlate = new THREE.Mesh(new THREE.PlaneGeometry(0.5, 0.125),
      new THREE.MeshBasicMaterial({ map: openTex, side: THREE.DoubleSide }));
    openPlate.position.set(0, 0.62, 0.04);
    doorGlassL.add(openPlate);
    refs.doorL = doorGlassL; refs.doorR = doorGlassR;
    refs.doorPose = { lC: 0.18, lO: -0.32, rC: 0.84, rO: 1.34 };
    refs.animDoor = { t: 0, state: 0, wait: 0 };

    // ---- 主招牌（发光灯箱） ----
    const signTex = App.signMainTex();
    const signMat = new THREE.MeshBasicMaterial({ map: signTex });
    const signBox = B(4.6, 0.5, 0.18, App.toon('#22315c'), -0.6, 2.62, -0.3, 0.02);
    const signFace = new THREE.Mesh(new THREE.PlaneGeometry(4.4, 0.4), signMat);
    signFace.position.set(0, 0, 0.125);
    signBox.add(signFace);
    refs.signMat = signMat;
    // 招牌下沿灯槽发光条
    refs.signGlow = P(4.6, 0.05, App.glow('#ffd98f', 1.2), -0.6, 2.6, -0.26);

    // ---- 拐角竖招牌（会闪烁） ----
    const vTex = App.signVertTex();
    const vMat = new THREE.MeshBasicMaterial({ map: vTex });
    const vSign = B(0.62, 2.5, 0.62, App.toon('#1a2440'), 2.28, 1.15, -0.42, 0.025);
    const vFace1 = new THREE.Mesh(new THREE.PlaneGeometry(0.54, 2.4), vMat);
    vFace1.rotation.y = Math.PI / 2; vFace1.position.x = 0.34;
    const vFace2 = vFace1.clone(); vFace2.rotation.y = -Math.PI / 2; vFace2.position.x = -0.34;
    const vFace3 = vFace1.clone(); vFace3.rotation.y = 0; vFace3.position.set(0, 0, 0.34); vFace3.rotation.y = 0;
    vSign.add(vFace1, vFace2, vFace3);
    refs.flicker.push(vMat);
    refs.vSignMat = vMat;

    // ---- 雨棚（条纹布 + 前沿滴水槽） ----
    const awningTex = App.canvasTex(256, 128, (ctx, w, h) => {
      for (let i = 0; i < 8; i++) {
        ctx.fillStyle = i % 2 ? '#e8e4d4' : '#3d7a5c';
        ctx.fillRect(i * w / 8, 0, w / 8, h);
      }
    });
    const awMat = new THREE.MeshToonMaterial({ map: awningTex, gradientMap: App.gradientMap });
    const awning = new THREE.Mesh(new THREE.BoxGeometry(3.1, 0.06, 0.85), awMat);
    awning.position.set(0.51, 2.28, 0.02);
    awning.rotation.x = 0.1;
    awning.castShadow = true;
    g.add(awning); App.outline(awning, 0.02);
    // 前沿垂帘
    const flap = new THREE.Mesh(new THREE.BoxGeometry(3.1, 0.16, 0.04), awMat);
    flap.position.set(0.51, 2.2, 0.435);
    g.add(flap); App.outline(flap, 0.018);
    // 支撑杆
    for (const sx of [-1.0, 2.0]) {
      const bar = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.02, 0.5, 6), steelMat);
      bar.position.set(sx + 0.51, 2.0, -0.28);
      bar.rotation.x = -0.6;
      g.add(bar);
    }
    // 滴水点（供 effects 使用）：雨棚前沿
    refs.dripPoints = [];
    for (let i = 0; i < 6; i++) {
      refs.dripPoints.push(new THREE.Vector3(0.51 - 1.45 + i * 0.58, 2.14, 0.44));
    }

    // ---- 屋顶 ----
    const roofMat = App.toon('#323950');
    B(6.2, 0.16, 3.2, roofMat, -0.9, 3.0, -1.8, 0.02);
    // 女儿墙压顶
    B(6.3, 0.1, 0.16, trimMat, -0.9, 3.16, -0.25, 0.015);
    B(6.3, 0.1, 0.16, trimMat, -0.9, 3.16, -3.35, 0.015);
    B(0.16, 0.1, 3.2, trimMat, -4.0, 3.16, -1.8, 0.015);
    B(0.16, 0.1, 3.2, trimMat, 2.2, 3.16, -1.8, 0.015);
    // 屋顶：空调外机 ×2、通风管、天线
    const acUnit = (x, z, ry, s) => {
      const grp = new THREE.Group();
      const body = new THREE.Mesh(App.roundBox(0.72, 0.55, 0.3, 0.02), App.toon('#b8bfc9'));
      body.castShadow = true; grp.add(body); App.outline(body, 0.018);
      const fan = new THREE.Mesh(new THREE.CylinderGeometry(0.19, 0.19, 0.05, 18), App.toon('#5a6272'));
      fan.rotation.x = Math.PI / 2; fan.position.z = 0.16; grp.add(fan);
      const grill = new THREE.Mesh(new THREE.BoxGeometry(0.4, 0.4, 0.02), App.toon('#727a8a'));
      grill.position.z = 0.17; grp.add(grill);
      grp.position.set(x, 3.16 + 0.275, z);
      if (ry) grp.rotation.y = ry;
      if (s) grp.scale.setScalar(s);
      g.add(grp);
      return grp;
    };
    acUnit(-2.4, -2.6, 0, 1);
    acUnit(-1.4, -2.6, 0, 0.85);
    acUnit(1.2, -1.2, Math.PI / 2, 0.9); // 右墙屋檐下的壁挂机
    const pipe = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 0.5, 8), steelMat);
    pipe.position.set(0.6, 3.4, -2.8); g.add(pipe);
    const ant = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.012, 1.0, 6), steelMat);
    ant.position.set(-3.2, 3.7, -2.9); g.add(ant);

    // ---- 外墙细节 ----
    // 右墙：海报 ×2、电表箱、雨水管
    const poster1 = new THREE.Mesh(new THREE.PlaneGeometry(0.42, 0.62), new THREE.MeshBasicMaterial({ map: App.posterTex(0) }));
    poster1.position.set(2.145, 1.7, -2.4); poster1.rotation.y = Math.PI / 2; g.add(poster1);
    const poster2 = new THREE.Mesh(new THREE.PlaneGeometry(0.42, 0.62), new THREE.MeshBasicMaterial({ map: App.posterTex(2) }));
    poster2.position.set(2.145, 1.7, -1.7); poster2.rotation.y = Math.PI / 2; g.add(poster2);
    const meter = B(0.34, 0.5, 0.16, App.toon('#9aa2b2'), 2.14, 1.0, -0.9, 0.015);
    const downpipe = new THREE.Mesh(new THREE.CylinderGeometry(0.035, 0.035, 3.0, 8), App.toon('#c8bfa8'));
    downpipe.position.set(1.98, 1.62, -3.15); g.add(downpipe); App.outline(downpipe, 0.012);
    // 左墙：外机 ×2（巷道侧）
    const acW1 = new THREE.Mesh(App.roundBox(0.6, 0.5, 0.28, 0.02), App.toon('#a8aeb8'));
    acW1.position.set(-4.02, 2.0, -1.2); acW1.castShadow = true; g.add(acW1); App.outline(acW1, 0.018);
    const acW2 = acW1.clone(); acW2.position.set(-4.02, 1.0, -2.2); g.add(acW2);
    const pipeL = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 3.0, 8), App.toon('#c8bfa8'));
    pipeL.position.set(-3.94, 1.6, -0.55); g.add(pipeL);
    // 后门（后巷侧）+ 门灯
    const bdoor = B(0.72, 1.75, 0.07, App.toon('#3d4d6a'), -2.3, 0.12, -3.34, 0.015);
    const bdStep = new THREE.Mesh(new THREE.BoxGeometry(0.84, 0.08, 0.3), App.toon('#8a8f99'));
    bdStep.position.set(-2.3, 0.16, -3.5);
    g.add(bdStep);
    const bdLamp = new THREE.Mesh(new THREE.BoxGeometry(0.16, 0.09, 0.12), App.toon('#39404f'));
    bdLamp.position.set(-2.3, 2.05, -3.36);
    g.add(bdLamp);
    const bdGlow = new THREE.Mesh(new THREE.PlaneGeometry(0.12, 0.05), App.glow('#ffe2a8', 1.1));
    bdGlow.position.set(-2.3, 2.0, -3.31);
    g.add(bdGlow);
    const bdLight = new THREE.PointLight('#ffd9a0', 0.35, 2.2, 2);
    bdLight.position.set(-2.3, 1.9, -3.7);
    g.add(bdLight);

    // 后墙：小窗 + 管线
    const bwin = new THREE.Mesh(new THREE.PlaneGeometry(0.6, 0.5), new THREE.MeshBasicMaterial({ color: '#1a2233' }));
    bwin.position.set(0.4, 1.7, -3.185); g.add(bwin);
    const bframe = new THREE.Mesh(new THREE.BoxGeometry(0.7, 0.6, 0.04), App.toon('#5a6272'));
    bframe.position.set(0.4, 1.7, -3.19); g.add(bframe);
    bwin.position.z += 0.05;

    /* ---------- 店内 ---------- */
    const shelfMat = App.toon('#e0e6ee');
    const shelfEdge = App.toon('#4a5568');

    // 天花板 + 灯板
    const ceil = new THREE.Mesh(new THREE.BoxGeometry(5.9, 0.06, 2.85), App.toon('#e8e4da'));
    ceil.position.set(-0.9, 2.83, -1.82);
    g.add(ceil);
    refs.ceilLights = [];
    for (let i = 0; i < 4; i++) {
      const li = new THREE.Mesh(new THREE.BoxGeometry(1.15, 0.04, 0.5),
        new THREE.MeshBasicMaterial({ color: '#fff3d8' }));
      li.position.set(-2.5 + i * 1.25, 2.79, -1.85);
      g.add(li);
      refs.ceilLights.push(li.material);
    }
    // 悬挂促销横幅
    const bannerTex = App.canvasTex(512, 128, (ctx, w, h) => {
      ctx.fillStyle = '#e8554d'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = '#fff'; ctx.font = `900 64px ${App.FONT}`; ctx.textAlign = 'center';
      ctx.fillText('冬のあったかフェア', w / 2, 86);
    });
    for (const bx of [-2.4, 0.3]) {
      const ban = new THREE.Mesh(new THREE.PlaneGeometry(1.5, 0.375),
        new THREE.MeshBasicMaterial({ map: bannerTex, side: THREE.DoubleSide }));
      ban.position.set(bx, 2.35, -1.0);
      g.add(ban);
    }

    // 靠墙货架（后墙整排，高 2.2）
    const buildWallShelf = () => {
      const grp = new THREE.Group();
      const back = new THREE.Mesh(new THREE.BoxGeometry(5.6, 2.2, 0.08), App.toon('#d8dee8'));
      back.position.set(0, 1.1, -0.2); grp.add(back); App.outline(back, 0.015);
      for (let s = 0; s < 5; s++) {
        const y = 0.25 + s * 0.44;
        const sh = new THREE.Mesh(new THREE.BoxGeometry(5.6, 0.045, 0.52), s % 2 ? shelfMat : App.toon('#e8edf4'));
        sh.position.set(0, y, 0); grp.add(sh);
        // 商品
        const n = 22;
        const inst = new THREE.InstancedMesh(new THREE.BoxGeometry(0.17, 0.26, 0.14),
          new THREE.MeshToonMaterial({ gradientMap: App.gradientMap }), n);
        const dummy = new THREE.Object3D();
        const col = new THREE.Color();
        for (let i = 0; i < n; i++) {
          dummy.position.set(-2.65 + i * 0.245, y + 0.16, -0.08 + Math.random() * 0.05);
          dummy.rotation.y = (Math.random() - 0.5) * 0.2;
          dummy.updateMatrix();
          inst.setMatrixAt(i, dummy.matrix);
          col.setHSL(Math.random(), 0.5, 0.72);
          inst.setColorAt(i, col);
        }
        inst.castShadow = true;
        grp.add(inst);
      }
      // 顶部灯箱广告
      const lb = new THREE.Mesh(new THREE.PlaneGeometry(2.2, 0.5),
        new THREE.MeshBasicMaterial({ map: App.canvasTex(512, 116, (ctx, w, h) => {
          ctx.fillStyle = '#2c4a8f'; ctx.fillRect(0, 0, w, h);
          ctx.fillStyle = '#ffe28a'; ctx.font = `900 62px ${App.FONT}`; ctx.textAlign = 'center';
          ctx.fillText('新商品 続々入荷', w / 2, 78);
        }) }));
      lb.position.set(0.6, 2.5, 0.1);
      grp.add(lb);
      grp.position.set(-0.9, 0.18, -3.02);
      return grp;
    };
    g.add(buildWallShelf());

    // 中岛双面货架 ×2 排
    const gondola = (z, x0, x1) => {
      const grp = new THREE.Group();
      const len = x1 - x0;
      const base = new THREE.Mesh(new THREE.BoxGeometry(len, 0.18, 0.8), App.toon('#4a5568'));
      base.position.set(0, 0.09, 0); grp.add(base); App.outline(base, 0.015);
      const backP = new THREE.Mesh(new THREE.BoxGeometry(len, 1.25, 0.05), App.toon('#d8dee8'));
      backP.position.set(0, 0.8, 0); grp.add(backP);
      for (const side of [-1, 1]) {
        for (let s = 0; s < 3; s++) {
          const y = 0.32 + s * 0.34;
          const sh = new THREE.Mesh(new THREE.BoxGeometry(len, 0.04, 0.34), shelfMat);
          sh.position.set(0, y, side * 0.23); grp.add(sh);
          const n = Math.floor(len / 0.22);
          const inst = new THREE.InstancedMesh(new THREE.BoxGeometry(0.13, 0.2, 0.12),
            new THREE.MeshToonMaterial({ gradientMap: App.gradientMap }), n);
          const dummy = new THREE.Object3D(), col = new THREE.Color();
          for (let i = 0; i < n; i++) {
            dummy.position.set(x0 + 0.14 + i * 0.22, y + 0.13, side * 0.23);
            dummy.rotation.y = (Math.random() - 0.5) * 0.3;
            dummy.updateMatrix();
            inst.setMatrixAt(i, dummy.matrix);
            col.setHSL(Math.random(), 0.55, 0.7);
            inst.setColorAt(i, col);
          }
          inst.castShadow = true;
          grp.add(inst);
        }
      }
      // 两端端头架
      for (const ex of [-len / 2 - 0.02, len / 2 + 0.02]) {
        const end = new THREE.Mesh(new THREE.BoxGeometry(0.05, 1.4, 0.8), shelfEdge);
        end.position.set(ex, 0.7, 0); grp.add(end);
      }
      grp.position.set((x0 + x1) / 2, 0.18, z);
      return grp;
    };
    g.add(gondola(-1.85, -2.9, 0.4));
    g.add(gondola(-1.3, -2.5, -0.1));

    // 饮料柜（右墙内侧，发光）
    const coolerGrp = new THREE.Group();
    const coolerBody = new THREE.Mesh(new THREE.BoxGeometry(0.65, 2.0, 2.0), App.toon('#cfd8e4'));
    coolerBody.position.set(0, 1.0, 0); coolerGrp.add(coolerBody); App.outline(coolerBody, 0.015);
    const coolerIn = new THREE.Mesh(new THREE.BoxGeometry(0.5, 1.7, 1.8),
      new THREE.MeshBasicMaterial({ color: '#dfeeff' }));
    coolerIn.position.set(-0.1, 1.05, 0); coolerGrp.add(coolerIn);
    for (let s = 0; s < 4; s++) {
      const sh = new THREE.Mesh(new THREE.BoxGeometry(0.4, 0.03, 1.75), App.toon('#eef4fa'));
      sh.position.set(-0.22, 0.45 + s * 0.42, 0); coolerGrp.add(sh);
      const n = 14;
      const inst = new THREE.InstancedMesh(new THREE.BoxGeometry(0.1, 0.22, 0.1),
        new THREE.MeshToonMaterial({ gradientMap: App.gradientMap }), n);
      const dummy = new THREE.Object3D(), col = new THREE.Color();
      for (let i = 0; i < n; i++) {
        dummy.position.set(-0.24, 0.58 + s * 0.42, -0.8 + i * 0.12);
        dummy.updateMatrix();
        inst.setMatrixAt(i, dummy.matrix);
        col.setHSL(Math.random(), 0.6, 0.65);
        inst.setColorAt(i, col);
      }
      coolerGrp.add(inst);
    }
    // 柜门玻璃
    const cGlass = new THREE.Mesh(new THREE.PlaneGeometry(1.85, 1.75),
      new THREE.MeshPhongMaterial({ color: '#bcd8f0', transparent: true, opacity: 0.2, shininess: 120, specular: '#aaccff', depthWrite: false }));
    cGlass.rotation.y = -Math.PI / 2;
    cGlass.position.set(-0.38, 1.05, 0);
    coolerGrp.add(cGlass);
    // 门贴
    const cSticker = new THREE.Mesh(new THREE.PlaneGeometry(0.5, 0.18),
      new THREE.MeshBasicMaterial({ map: App.coolerTex(), side: THREE.DoubleSide }));
    cSticker.rotation.y = -Math.PI / 2;
    cSticker.position.set(-0.395, 1.85, 0);
    coolerGrp.add(cSticker);
    coolerGrp.position.set(1.6, 0.18, -2.0);
    g.add(coolerGrp);

    // 便当柜（左前窗边，暖光）
    const bentoGrp = new THREE.Group();
    const bBase = new THREE.Mesh(new THREE.BoxGeometry(0.9, 0.85, 0.55), App.toon('#d8dee8'));
    bBase.position.set(0, 0.42, 0); bentoGrp.add(bBase); App.outline(bBase, 0.015);
    const bCase = new THREE.Mesh(new THREE.BoxGeometry(0.8, 0.5, 0.45),
      new THREE.MeshBasicMaterial({ color: '#ffe9c4', transparent: true, opacity: 0.25 }));
    bCase.position.set(0, 1.1, 0); bentoGrp.add(bCase);
    const bGlass = new THREE.Mesh(new THREE.BoxGeometry(0.86, 0.46, 0.5),
      new THREE.MeshPhongMaterial({ color: '#ffe9c4', transparent: true, opacity: 0.15, shininess: 120, specular: '#ffcc88', depthWrite: false }));
    bGlass.position.set(0, 1.1, 0); bentoGrp.add(bGlass);
    for (let i = 0; i < 6; i++) {
      const b = new THREE.Mesh(new THREE.BoxGeometry(0.24, 0.09, 0.17),
        App.toon(i % 2 ? '#e87a6a' : '#f0c060'));
      b.position.set(-0.26 + (i % 3) * 0.26, 0.98 + Math.floor(i / 3) * 0.22, 0);
      bentoGrp.add(b);
    }
    // 便当柜灯带
    const bLight = new THREE.Mesh(new THREE.BoxGeometry(0.8, 0.03, 0.4), App.glow('#ffdf9f', 1.3));
    bLight.position.set(0, 1.375, 0); bentoGrp.add(bLight);
    bentoGrp.position.set(-2.1, 0.18, -0.85);
    bentoGrp.rotation.y = 0;
    g.add(bentoGrp);

    // 关东煮柜台（更靠左）
    const odenGrp = new THREE.Group();
    const oBase = new THREE.Mesh(new THREE.BoxGeometry(0.75, 0.9, 0.5), App.toon('#c8442e'));
    oBase.position.set(0, 0.45, 0); odenGrp.add(oBase); App.outline(oBase, 0.015);
    const oTop = new THREE.Mesh(new THREE.BoxGeometry(0.68, 0.12, 0.44),
      new THREE.MeshBasicMaterial({ color: '#ffdf9f' }));
    oTop.position.set(0, 0.94, 0); odenGrp.add(oTop);
    // 关东煮锅
    for (let i = 0; i < 2; i++) {
      const pot = new THREE.Mesh(new THREE.CylinderGeometry(0.14, 0.14, 0.1, 14),
        App.toon('#d8dde4'));
      pot.position.set(-0.16 + i * 0.32, 1.05, 0);
      odenGrp.add(pot);
      const soup = new THREE.Mesh(new THREE.CylinderGeometry(0.12, 0.12, 0.02, 14),
        App.glow('#e8b96a', 1.2));
      soup.position.set(-0.16 + i * 0.32, 1.118, 0);
      odenGrp.add(soup);
    }
    const oGlass = new THREE.Mesh(new THREE.BoxGeometry(0.8, 0.5, 0.46),
      new THREE.MeshPhongMaterial({ color: '#ffd9a0', transparent: true, opacity: 0.14, shininess: 110, specular: '#ffcc88', depthWrite: false }));
    oGlass.position.set(0, 1.08, 0); odenGrp.add(oGlass);
    odenGrp.position.set(-3.15, 0.18, -0.85);
    g.add(odenGrp);

    // 杂志架（左墙）
    const magGrp = new THREE.Group();
    const magBody = new THREE.Mesh(new THREE.BoxGeometry(0.4, 1.5, 1.1), App.toon('#d8dee8'));
    magBody.position.set(0, 0.75, 0); magGrp.add(magBody); App.outline(magBody, 0.015);
    for (let i = 0; i < 8; i++) {
      const mag = new THREE.Mesh(new THREE.PlaneGeometry(0.3, 0.42),
        new THREE.MeshBasicMaterial({ map: App.magazineTex(i) }));
      mag.rotation.y = Math.PI / 2;
      mag.position.set(0.21, 0.5 + Math.floor(i / 2) * 0.34, -0.4 + (i % 2) * 0.42 + 0.21);
      magGrp.add(mag);
    }
    magGrp.position.set(-3.55, 0.18, -2.1);
    g.add(magGrp);

    // 收银台（门左侧）
    const regGrp = new THREE.Group();
    const counter = new THREE.Mesh(App.roundBox(1.3, 0.95, 0.6, 0.02), App.toon('#e8e2d4'));
    counter.position.set(0, 0.475, 0); regGrp.add(counter); App.outline(counter, 0.015);
    const cTop = new THREE.Mesh(new THREE.BoxGeometry(1.36, 0.05, 0.66), App.toon('#4a5568'));
    cTop.position.set(0, 0.97, 0); regGrp.add(cTop);
    // 收银机
    const reg = new THREE.Mesh(new THREE.BoxGeometry(0.35, 0.3, 0.3), App.toon('#d8dde4'));
    reg.position.set(0.35, 1.14, -0.1); regGrp.add(reg); App.outline(reg, 0.012);
    const regScr = new THREE.Mesh(new THREE.PlaneGeometry(0.26, 0.14), App.glow('#bfe8d0', 1.1));
    regScr.position.set(0.35, 1.17, 0.075); regGrp.add(regScr);
    // 咖啡机
    const cof = new THREE.Mesh(App.roundBox(0.3, 0.42, 0.28, 0.02), App.toon('#8a4a3a'));
    cof.position.set(-0.4, 1.0, -0.1); regGrp.add(cof); App.outline(cof, 0.012);
    const cofGlow = new THREE.Mesh(new THREE.PlaneGeometry(0.2, 0.16), App.glow('#ff9a5a', 1.2));
    cofGlow.position.set(-0.4, 1.08, 0.065); regGrp.add(cofGlow);
    // 烟柜背架
    const smoke = new THREE.Mesh(new THREE.BoxGeometry(1.0, 0.8, 0.25), App.toon('#6a7284'));
    smoke.position.set(0.1, 1.4, -0.45); regGrp.add(smoke);
    regGrp.position.set(-0.95, 0.18, -0.75);
    g.add(regGrp);

    // 背墙海报
    for (let i = 0; i < 2; i++) {
      const p = new THREE.Mesh(new THREE.PlaneGeometry(0.42, 0.6),
        new THREE.MeshBasicMaterial({ map: App.posterTex(i + 1) }));
      p.position.set(-3.0 + i * 4.4, 2.55, -3.15);
      g.add(p);
    }

    // 店内地贴导视
    const guide = new THREE.Mesh(new THREE.PlaneGeometry(0.8, 0.5),
      new THREE.MeshBasicMaterial({ map: App.canvasTex(256, 160, (ctx, w, h) => {
        ctx.fillStyle = '#f0e8d8'; ctx.fillRect(0, 0, w, h);
        ctx.fillStyle = '#2c8f5f'; ctx.fillRect(10, 10, w - 20, h - 20);
        ctx.fillStyle = '#fff'; ctx.font = `900 64px ${App.FONT}`; ctx.textAlign = 'center';
        ctx.fillText('レジ', w / 2, h / 2 + 24);
      }) }));
    guide.rotation.x = -Math.PI / 2;
    guide.position.set(-0.95, 0.2, -0.35);
    g.add(guide);

    /* ---------- 店内灯光 ---------- */
    const l1 = new THREE.PointLight('#ffd9a8', 1.35, 8.0, 2);
    l1.position.set(-0.9, 2.55, -1.7);
    g.add(l1);
    const l2 = new THREE.PointLight('#ffe4bc', 0.95, 6.5, 2);
    l2.position.set(-2.2, 2.3, -0.9);
    g.add(l2);
    const l3 = new THREE.PointLight('#cfe4ff', 0.75, 3.8, 2);
    l3.position.set(1.3, 1.7, -2.0);
    g.add(l3);
    refs.interiorLights = [l1, l2];
    refs.coolerLight = l3;

    root.add(g);
    return refs;
  };

})(window.App);
