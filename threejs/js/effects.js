/* =========================================================
 * effects.js — 雨夜动效：降雨 / 滴水 / 涟漪 / 招牌闪烁 /
 *              自动门 / 玻璃雨水 / 红绿灯
 * ========================================================= */
'use strict';
(function (App) {

  App.buildEffects = function (scene, storeRefs, streetRefs) {
    const fx = {};
    const rand = (a, b) => a + Math.random() * (b - a);

    /* ================= 持续降雨 ================= */
    const RAIN_N = 1500;
    const RAIN_AREA = 4.9, RAIN_TOP = 7.5;
    const rainPos = new Float32Array(RAIN_N * 6);
    const rainVel = new Float32Array(RAIN_N);
    const rainLen = new Float32Array(RAIN_N);
    for (let i = 0; i < RAIN_N; i++) {
      const x = rand(-RAIN_AREA, RAIN_AREA);
      const y = rand(0, RAIN_TOP);
      const z = rand(-RAIN_AREA, RAIN_AREA);
      rainPos[i * 6] = x; rainPos[i * 6 + 1] = y; rainPos[i * 6 + 2] = z;
      rainLen[i] = rand(0.22, 0.42);
      rainVel[i] = rand(6.5, 9.5);
      rainPos[i * 6 + 3] = x + 0.03;
      rainPos[i * 6 + 4] = y + rainLen[i];
      rainPos[i * 6 + 5] = z;
    }
    const rainGeo = new THREE.BufferGeometry();
    rainGeo.setAttribute('position', new THREE.BufferAttribute(rainPos, 3));
    const rainMat = new THREE.LineBasicMaterial({
      color: '#a8c0e0', transparent: true, opacity: 0.4, depthWrite: false,
    });
    const rain = new THREE.LineSegments(rainGeo, rainMat);
    rain.frustumCulled = false;
    scene.add(rain);

    fx.updateRain = function (dt) {
      const arr = rainGeo.attributes.position.array;
      for (let i = 0; i < RAIN_N; i++) {
        const o = i * 6;
        const v = rainVel[i] * dt;
        arr[o + 1] -= v;
        arr[o + 4] -= v;
        arr[o] -= 0.5 * dt;      // 微风
        arr[o + 3] -= 0.5 * dt;
        if (arr[o + 1] < 0.02) {
          const x = rand(-RAIN_AREA, RAIN_AREA);
          const z = rand(-RAIN_AREA, RAIN_AREA);
          const y = RAIN_TOP - rand(0, 0.6);
          const L = rainLen[i];
          arr[o] = x; arr[o + 1] = y; arr[o + 2] = z;
          arr[o + 3] = x + 0.03; arr[o + 4] = y + L; arr[o + 5] = z;
        }
      }
      rainGeo.attributes.position.needsUpdate = true;
    };

    /* ================= 地面溅落小水花 ================= */
    const splashPool = [];
    const splashGeo = new THREE.RingGeometry(0.028, 0.042, 12);
    for (let i = 0; i < 30; i++) {
      const m = new THREE.Mesh(splashGeo, new THREE.MeshBasicMaterial({
        color: '#b8cce8', transparent: true, opacity: 0, depthWrite: false,
      }));
      m.rotation.x = -Math.PI / 2;
      m.visible = false;
      m.renderOrder = 6;
      scene.add(m);
      splashPool.push({ mesh: m, t: 1, dur: 0.5 });
    }
    let splashTimer = 0;
    const splashZones = [
      { x: [-4.6, 2.8], z: [0.75, 3.2], w: 5 },   // 前路
      { x: [3.05, 4.6], z: [-4.6, 4.6], w: 3 },   // 右路
      { x: [-4.6, 2.8], z: [-0.25, 0.55], w: 2 }, // 店前人行道
      { x: [-4.6, 4.6], z: [3.45, 4.65], w: 2 },  // 对面人行道
      { x: [2.2, 2.9], z: [-4.6, 0.5], w: 1.5 },  // 右人行道
    ];
    const pickSplash = () => {
      let total = 0;
      for (const s of splashZones) total += s.w;
      let r = Math.random() * total;
      for (const s of splashZones) {
        r -= s.w;
        if (r <= 0) return s;
      }
      return splashZones[0];
    };
    fx.updateSplash = function (dt) {
      splashTimer -= dt;
      if (splashTimer <= 0) {
        splashTimer = 0.028;
        for (const s of splashPool) {
          if (s.t >= s.dur) {
            const zone = pickSplash();
            s.mesh.position.set(rand(zone.x[0], zone.x[1]), 0.055, rand(zone.z[0], zone.z[1]));
            s.mesh.scale.setScalar(rand(0.7, 1.4));
            s.t = 0;
            s.mesh.visible = true;
            break;
          }
        }
      }
      for (const s of splashPool) {
        if (s.t < s.dur) {
          s.t += dt;
          const k = s.t / s.dur;
          s.mesh.scale.setScalar(s.mesh.scale.x + dt * 1.6);
          s.mesh.material.opacity = 0.36 * (1 - k);
          if (k >= 1) s.mesh.visible = false;
        }
      }
    };

    /* ================= 积水涟漪 ================= */
    const ripplePool = [];
    const rippleSpots = streetRefs.puddleSpots || [];
    for (let i = 0; i < 14; i++) {
      const m = new THREE.Mesh(new THREE.RingGeometry(0.05, 0.075, 20), new THREE.MeshBasicMaterial({
        color: '#9fb8dd', transparent: true, opacity: 0, depthWrite: false,
      }));
      m.rotation.x = -Math.PI / 2;
      m.visible = false;
      m.renderOrder = 6;
      scene.add(m);
      ripplePool.push({ mesh: m, t: 1, dur: 1.7 });
    }
    let rippleTimer = 0.8, rippleIdx = 0;
    fx.updateRipples = function (dt) {
      if (!rippleSpots.length) return;
      rippleTimer -= dt;
      if (rippleTimer <= 0) {
        rippleTimer = 0.5;
        const spot = rippleSpots[rippleIdx % rippleSpots.length];
        rippleIdx++;
        for (const r of ripplePool) {
          if (r.t >= r.dur) {
            r.mesh.position.set(
              spot.x + rand(-0.4, 0.4) * (spot === rippleSpots[0] ? 1 : 0.6),
              spot.y + 0.004,
              spot.z + rand(-0.3, 0.3) * (spot === rippleSpots[0] ? 1 : 0.5)
            );
            r.mesh.scale.setScalar(rand(0.8, 1.5));
            r.t = 0;
            r.mesh.visible = true;
            break;
          }
        }
      }
      for (const r of ripplePool) {
        if (r.t < r.dur) {
          r.t += dt;
          const k = r.t / r.dur;
          r.mesh.scale.setScalar(r.mesh.scale.x * (1 + dt * 0.85));
          r.mesh.material.opacity = 0.5 * Math.pow(1 - k, 1.6);
          if (k >= 1) r.mesh.visible = false;
        }
      }
    };

    /* ================= 屋檐滴水 ================= */
    const dripPool = [];
    const dripGeo = new THREE.SphereGeometry(0.016, 6, 6);
    const dripMat = new THREE.MeshBasicMaterial({ color: '#cfe2f5', transparent: true, opacity: 0.85 });
    const dripPoints = storeRefs.dripPoints || [];
    dripPoints.forEach((p, i) => {
      dripPool.push({
        p: p.clone(),
        next: rand(0.3, 2.5) + i * 0.37,
        mesh: null,
      });
    });
    for (let i = 0; i < 10; i++) {
      const m = new THREE.Mesh(dripGeo, dripMat);
      m.visible = false;
      m.scale.y = 2.2;
      scene.add(m);
      dripPool.push({ free: m });
    }
    fx.updateDrips = function (dt) {
      for (const d of dripPool) {
        if (d.p && !d.mesh) {
          d.next -= dt;
          if (d.next <= 0) {
            for (const o of dripPool) {
              if (o.free && !o.busy) {
                o.busy = true;
                o.mesh = o.free;
                o.x = d.p.x + rand(-0.015, 0.015);
                o.z = d.p.z + rand(-0.015, 0.015);
                o.y = d.p.y;
                o.v = 0.4;
                o.mesh.visible = true;
                d.next = rand(0.9, 2.6);
                break;
              }
            }
          }
        }
      }
      for (const o of dripPool) {
        if (o.busy) {
          o.v += 7.5 * dt;
          o.y -= o.v * dt;
          if (o.y <= 0.05) {
            o.busy = false;
            o.mesh.visible = false;
            // 落地涟漪
            for (const r of ripplePool) {
              if (r.t >= r.dur) {
                r.mesh.position.set(o.x, 0.05, o.z);
                r.mesh.scale.setScalar(0.55);
                r.t = 0;
                r.mesh.visible = true;
                break;
              }
            }
          } else {
            o.mesh.position.set(o.x, o.y, o.z);
          }
        }
      }
    };

    /* ================= 玻璃上的雨水滑落 ================= */
    const streakPool = [];
    const streakZones = [
      { x0: -3.44, x1: -0.24, z: -0.335 },
      { x0: 1.24, x1: 1.92, z: -0.335 },
    ];
    for (let i = 0; i < 26; i++) {
      const h = rand(0.14, 0.4);
      const m = new THREE.Mesh(new THREE.PlaneGeometry(0.016, h), new THREE.MeshBasicMaterial({
        color: '#d4e4f4', transparent: true, opacity: rand(0.18, 0.4), depthWrite: false,
      }));
      const zone = streakZones[i % streakZones.length];
      m.position.set(rand(zone.x0, zone.x1), rand(0.8, 2.4), zone.z);
      m.userData = { v: rand(0.1, 0.28), zone, wob: rand(0, Math.PI * 2) };
      m.renderOrder = 8;
      scene.add(m);
      streakPool.push(m);
    }
    fx.updateGlass = function (dt, t) {
      for (const m of streakPool) {
        m.position.y -= m.userData.v * dt;
        m.position.x += Math.sin(t * 1.4 + m.userData.wob) * 0.0006;
        if (m.position.y < 0.75) {
          m.position.y = rand(2.1, 2.45);
          m.position.x = rand(m.userData.zone.x0, m.userData.zone.x1);
          m.material.opacity = rand(0.18, 0.4);
        }
      }
    };

    /* ================= 自动门开合 ================= */
    const door = storeRefs.animDoor;
    const doorPose = storeRefs.doorPose;
    fx.updateDoor = function (dt) {
      if (!door) return;
      door.t += dt;
      let k = 0; // 0 关 1 开
      if (door.state === 0) {            // 关闭等待
        if (door.t > rand(4.5, 8)) { door.state = 1; door.t = 0; }
      } else if (door.state === 1) {     // 开启中
        k = Math.min(door.t / 1.1, 1);
        if (k >= 1) { door.state = 2; door.t = 0; }
      } else if (door.state === 2) {     // 敞开
        k = 1;
        if (door.t > 2.8) { door.state = 3; door.t = 0; }
      } else {                            // 关闭中
        k = 1 - Math.min(door.t / 1.1, 1);
        if (k <= 0) { door.state = 0; door.t = 0; k = 0; }
      }
      const e = k * k * (3 - 2 * k); // smoothstep
      if (storeRefs.doorL) {
        storeRefs.doorL.position.x = doorPose.lC + (doorPose.lO - doorPose.lC) * e;
        storeRefs.doorR.position.x = doorPose.rC + (doorPose.rO - doorPose.rC) * e;
      }
    };

    /* ================= 招牌 / 灯效闪烁 ================= */
    const vMat = storeRefs.vSignMat;
    let flickerNext = rand(2, 5), flickerEnd = -1;
    fx.updateLights = function (dt, t) {
      // 竖招牌偶发接触不良式闪烁
      if (vMat) {
        flickerNext -= dt;
        let f = 1;
        if (flickerNext <= 0 && flickerEnd < 0) {
          flickerEnd = rand(0.15, 0.45);
          flickerNext = rand(4, 9);
        }
        if (flickerEnd >= 0) {
          flickerEnd -= dt;
          f = Math.random() < 0.4 ? 0.5 : 1;
          if (flickerEnd < 0) f = 1;
        }
        f *= 0.96 + 0.04 * Math.sin(t * 37);
        vMat.color.setScalar(f);
      }
      // 主招牌灯槽呼吸
      if (storeRefs.signGlow) {
        storeRefs.signGlow.material.color.setScalar(0.9 + 0.1 * Math.sin(t * 2.2));
      }
      // 便当柜灯微闪
      if (storeRefs.ceilLights) {
        const b = 0.97 + 0.03 * Math.sin(t * 31);
        for (const m of storeRefs.ceilLights) m.color.setRGB(1 * b, 0.953 * b, 0.847 * b);
      }
    };

    /* ================= 红绿灯 ================= */
    const tl = streetRefs.trafficMats;
    const poolMat = streetRefs.trafficPoolMat;
    const ON = { r: '#ff6a55', y: '#ffd257', g: '#66f08a' };
    const OFF = '#161b26';
    let tlT = 0;
    const CYCLE = [['g', 4.0], ['y', 1.3], ['r', 4.6]];
    fx.updateTraffic = function (dt) {
      tlT += dt;
      let acc = 0, key = 'r';
      for (const [k, d] of CYCLE) {
        acc += d;
        if (tlT % 9.9 < acc) { key = k; break; }
      }
      for (const k of ['r', 'y', 'g']) {
        tl[k].color.set(k === key ? ON[k] : OFF);
      }
      if (poolMat) {
        const c = new THREE.Color(ON[key]).multiplyScalar(0.85);
        poolMat.color.lerp(c, Math.min(dt * 6, 1));
        poolMat.opacity = 0.22 + 0.08 * Math.sin(t_global * 3) * 0.5;
      }
    };

    let t_global = 0;
    fx.update = function (dt, t) {
      t_global = t;
      fx.updateRain(dt);
      fx.updateSplash(dt);
      fx.updateRipples(dt);
      fx.updateDrips(dt);
      fx.updateGlass(dt, t);
      fx.updateDoor(dt);
      fx.updateLights(dt, t);
      fx.updateTraffic(dt);
    };

    return fx;
  };

})(window.App);
