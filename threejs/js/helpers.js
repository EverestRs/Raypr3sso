/* =========================================================
 * helpers.js — 三渲二风格工具库：卡通材质 / 描边 / 程序化贴图
 * ========================================================= */
'use strict';
window.App = window.App || {};

(function (App) {

  /* ---------- 全局共享的卡通渐变贴图（4 阶） ---------- */
  const gradData = new Uint8Array([110, 165, 215, 255]);
  const gradientMap = new THREE.DataTexture(gradData, 4, 1, THREE.RedFormat);
  gradientMap.minFilter = THREE.NearestFilter;
  gradientMap.magFilter = THREE.NearestFilter;
  gradientMap.generateMipmaps = false;
  gradientMap.needsUpdate = true;
  App.gradientMap = gradientMap;

  /* ---------- 材质工厂 ---------- */
  // 卡通材质：flat 色块 + 阶梯明暗
  App.toon = function (color, opt) {
    opt = opt || {};
    const m = new THREE.MeshToonMaterial({
      color: color,
      gradientMap: gradientMap,
    });
    if (opt.emissive !== undefined) {
      m.emissive = new THREE.Color(opt.emissive);
      m.emissiveIntensity = opt.emissiveIntensity !== undefined ? opt.emissiveIntensity : 1;
    }
    if (opt.map) m.map = opt.map;
    if (opt.emissiveMap) { m.emissiveMap = opt.emissiveMap; m.emissive = m.emissive || new THREE.Color('#ffffff'); if (opt.emissiveIntensity !== undefined) m.emissiveIntensity = opt.emissiveIntensity; }
    if (opt.transparent) { m.transparent = true; m.opacity = opt.opacity !== undefined ? opt.opacity : 1; }
    if (opt.side) m.side = opt.side;
    return m;
  };

  // 湿面材质：Phong 高光，用于沥青 / 积水等需要"灯光倒影"的表面
  App.wet = function (color, opt) {
    opt = opt || {};
    return new THREE.MeshPhongMaterial({
      color: color,
      shininess: opt.shininess !== undefined ? opt.shininess : 90,
      specular: opt.specular !== undefined ? opt.specular : new THREE.Color('#5a6f8f'),
      emissive: opt.emissive !== undefined ? opt.emissive : 0x000000,
      transparent: !!opt.transparent,
      opacity: opt.opacity !== undefined ? opt.opacity : 1,
    });
  };

  // 自发光平面/灯箱
  App.glow = function (color, intensity) {
    return new THREE.MeshBasicMaterial({ color: new THREE.Color(color).multiplyScalar(intensity || 1) });
  };

  /* ---------- 倒角立方体几何（柔和的模型感） ---------- */
  App.roundBox = function (w, h, d, r, seg) {
    r = Math.min(r, w / 2 - 1e-4, h / 2 - 1e-4, d / 2 - 1e-4);
    const shape = new THREE.Shape();
    const hw = w / 2, hd = d / 2;
    shape.moveTo(-hw + r, -hd);
    shape.lineTo(hw - r, -hd);
    shape.absarc(hw - r, -hd + r, r, -Math.PI / 2, 0, false);
    shape.lineTo(hw, hd - r);
    shape.absarc(hw - r, hd - r, r, 0, Math.PI / 2, false);
    shape.lineTo(-hw + r, hd);
    shape.absarc(-hw + r, hd - r, r, Math.PI / 2, Math.PI, false);
    shape.lineTo(-hw, -hd + r);
    shape.absarc(-hw + r, -hd + r, r, Math.PI, Math.PI * 1.5, false);
    const geo = new THREE.ExtrudeGeometry(shape, {
      depth: h - r * 2 > 0 ? h - r * 2 : 0.001,
      bevelEnabled: true,
      bevelThickness: r,
      bevelSize: r,
      bevelSegments: seg || 2,
      curveSegments: 6,
    });
    geo.rotateX(-Math.PI / 2);
    geo.translate(0, r - h / 2, 0); // 旋转后 y∈[-r, h-r]，平移回居中
    geo.computeVertexNormals();
    return geo;
  };

  /* ---------- 反向壳描边（三渲二轮廓线） ---------- */
  const outlineVert = `
    uniform float uW;
    #include <common>
    #include <logdepthbuf_pars_vertex>
    void main() {
      vec3 p = position + normalize(normal) * uW;
      gl_Position = projectionMatrix * modelViewMatrix * vec4(p, 1.0);
      #include <logdepthbuf_vertex>
    }`;
  const outlineFrag = `
    uniform vec3 uC;
    #include <logdepthbuf_pars_fragment>
    void main() {
      #include <logdepthbuf_fragment>
      gl_FragColor = vec4(uC, 1.0);
    }`;
  const outlineMatCache = {};
  App.outlineMat = function (w) {
    const key = w.toFixed(4);
    if (!outlineMatCache[key]) {
      outlineMatCache[key] = new THREE.ShaderMaterial({
        uniforms: { uW: { value: w }, uC: { value: new THREE.Color('#141824') } },
        vertexShader: outlineVert,
        fragmentShader: outlineFrag,
        side: THREE.BackSide,
      });
    }
    return outlineMatCache[key];
  };
  // 为 mesh 添加描边子对象（物体几何尺寸需在 geometry 中，而非靠 scale 拉伸）
  App.outline = function (mesh, w) {
    const o = new THREE.Mesh(mesh.geometry, App.outlineMat(w === undefined ? 0.022 : w));
    o.renderOrder = -1;
    mesh.add(o);
    return mesh;
  };

  /* ---------- 程序化 Canvas 贴图 ---------- */
  const FONT = '"Hiragino Sans","Yu Gothic","Noto Sans CJK JP","Noto Sans JP","Meiryo",sans-serif';
  App.FONT = FONT;

  App.canvasTex = function (w, h, draw) {
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    draw(c.getContext('2d'), w, h);
    const t = new THREE.CanvasTexture(c);
    t.encoding = THREE.sRGBEncoding;
    t.anisotropy = 4;
    return t;
  };

  // 竖排文字
  function vText(ctx, chars, x, y, size, color, lineH) {
    ctx.fillStyle = color;
    ctx.font = `bold ${size}px ${FONT}`;
    ctx.textAlign = 'center';
    let cy = y;
    for (const ch of chars) { ctx.fillText(ch, x, cy); cy += lineH || size * 1.08; }
  }

  // 便利店主招牌：暖白发光底 + 品牌圆标 + 日文店名
  App.signMainTex = function () {
    return App.canvasTex(1024, 256, (ctx, w, h) => {
      const g = ctx.createLinearGradient(0, 0, 0, h);
      g.addColorStop(0, '#fff8e8'); g.addColorStop(1, '#ffedc4');
      ctx.fillStyle = g; ctx.fillRect(0, 0, w, h);
      // 品牌圆标：夜空蓝 + 月牙 + 杯面热气感的圆
      ctx.fillStyle = '#2c4a8f';
      ctx.beginPath(); ctx.arc(120, h / 2, 92, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#ffe28a';
      ctx.beginPath(); ctx.arc(120, h / 2 - 18, 40, Math.PI * 0.15, Math.PI * 1.15, false); ctx.fill();
      ctx.beginPath(); ctx.arc(132, h / 2 + 22, 26, 0, Math.PI * 2); ctx.fill();
      // 店名
      ctx.fillStyle = '#22315c';
      ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
      ctx.font = `900 150px ${FONT}`;
      ctx.fillText('よる', 250, h / 2 - 34);
      ctx.font = `900 118px ${FONT}`;
      ctx.fillText('マート', 560, h / 2 - 30);
      ctx.font = `bold 52px ${FONT}`;
      ctx.fillStyle = '#a85a28';
      ctx.fillText('24時間営業', 258, h - 52);
      // 小时圆点
      ctx.fillStyle = '#e8554d';
      ctx.beginPath(); ctx.arc(880, h - 66, 44, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = '#fff'; ctx.font = `900 46px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText('24', 880, h - 60);
    });
  };

  // 拐角竖招牌
  App.signVertTex = function () {
    return App.canvasTex(256, 1024, (ctx, w, h) => {
      ctx.fillStyle = '#1f2f55'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = '#ffffff';
      ctx.fillRect(14, 14, w - 28, h - 28);
      ctx.fillStyle = '#e84a3f';
      ctx.fillRect(14, 14, w - 28, 190);
      ctx.fillStyle = '#fff'; ctx.font = `900 110px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText('OPEN', w / 2, 130);
      vText(ctx, ['夜', '間', '営', '業'], w / 2, 350, 150, '#22315c', 165);
      ctx.fillStyle = '#2c8f5f';
      ctx.fillRect(28, h - 250, w - 56, 210);
      ctx.fillStyle = '#fff'; ctx.font = `900 96px ${FONT}`;
      ctx.fillText('24時', w / 2, h - 118);
    });
  };

  // 墙上海报（多种配色）
  App.posterTex = function (variant) {
    const themes = [
      { bg: '#f2e9dc', main: '#d94f43', word: '新発売', sub: 'あったかおでん' },
      { bg: '#dcefe8', main: '#2e7a5f', word: '短缺中', sub: 'アイスコーヒー' },
      { bg: '#fdeccd', main: '#d88a2e', word: 'フェア', sub: '甘いスイーツ' },
      { bg: '#e3e9f7', main: '#3a5fb0', word: 'セール', sub: '毎週 金・土' },
    ];
    const t = themes[variant % themes.length];
    return App.canvasTex(256, 384, (ctx, w, h) => {
      ctx.fillStyle = t.bg; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = t.main;
      ctx.fillRect(0, 0, w, 110);
      ctx.fillRect(0, h - 90, w, 90);
      ctx.fillStyle = '#fff'; ctx.font = `900 62px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText(t.word, w / 2, 78);
      // 简单商品图形：饭团 / 咖啡杯 / 甜甜圈
      ctx.save();
      ctx.translate(w / 2, h / 2 + 10);
      ctx.fillStyle = t.main;
      if (variant % 4 === 0) { // 饭团
        ctx.beginPath(); ctx.moveTo(-70, 55); ctx.quadraticCurveTo(-70, -55, 0, -55);
        ctx.quadraticCurveTo(70, -55, 70, 55); ctx.closePath(); ctx.fill();
        ctx.fillStyle = '#fff'; ctx.fillRect(-70, 18, 140, 37);
      } else if (variant % 4 === 1) { // 咖啡杯
        ctx.fillRect(-40, -50, 80, 90);
        ctx.fillStyle = '#fff'; ctx.fillRect(-40, -20, 80, 16);
        ctx.beginPath(); ctx.arc(28, 0, 18, -0.6, 2.2); ctx.stroke();
        ctx.strokeStyle = '#fff'; ctx.beginPath(); ctx.arc(30, 2, 20, -0.7, 2.0); ctx.stroke();
      } else if (variant % 4 === 2) { // 甜甜圈
        ctx.beginPath(); ctx.arc(0, 0, 58, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = t.bg; ctx.beginPath(); ctx.arc(0, 0, 24, 0, Math.PI * 2); ctx.fill();
      } else { // 折扣标签
        ctx.beginPath(); ctx.moveTo(-60, -45); ctx.lineTo(60, -45); ctx.lineTo(60, 45);
        ctx.lineTo(0, 70); ctx.lineTo(-60, 45); ctx.closePath(); ctx.fill();
        ctx.fillStyle = '#fff'; ctx.font = `900 70px ${FONT}`;
        ctx.fillText('%', 0, 22);
      }
      ctx.restore();
      ctx.fillStyle = t.main; ctx.font = `bold 40px ${FONT}`;
      ctx.fillText(t.sub, w / 2, h - 40);
    });
  };

  // 杂志封面
  App.magazineTex = function (i) {
    const colors = ['#e85a4f', '#3a7bd5', '#f0a032', '#57b884', '#9a5fd0', '#2ba8a0'];
    const names = ['WEEKLY', 'モーニング', 'ヤング', 'VOLTAGE', 'きらら', 'DRIVE'];
    const c = colors[i % colors.length];
    return App.canvasTex(128, 176, (ctx, w, h) => {
      ctx.fillStyle = '#f5f2ea'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = c; ctx.fillRect(0, 0, w, 52);
      ctx.fillStyle = '#fff'; ctx.font = `900 26px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText(names[i % names.length], w / 2, 38);
      // 人像剪影风
      ctx.fillStyle = c; ctx.globalAlpha = 0.25;
      ctx.beginPath(); ctx.arc(w / 2, h * 0.52, 30, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.moveTo(w / 2 - 44, h - 10);
      ctx.quadraticCurveTo(w / 2, h * 0.55, w / 2 + 44, h - 10); ctx.fill();
      ctx.globalAlpha = 1;
      ctx.fillStyle = '#333'; ctx.font = `bold 18px ${FONT}`;
      ctx.fillText('第' + (i + 1) + '号', w / 2, h - 14);
    });
  };

  // 饮料柜门贴图
  App.coolerTex = function () {
    return App.canvasTex(256, 512, (ctx, w, h) => {
      ctx.fillStyle = '#dceefb'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = '#3a7bd5'; ctx.fillRect(0, 0, w, 64);
      ctx.fillStyle = '#fff'; ctx.font = `900 44px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText('つめた〜い', w / 2, 46);
      ctx.fillStyle = '#88a8c8'; ctx.font = `bold 26px ${FONT}`;
      ctx.fillText('BEVERAGE', w / 2, h - 20);
    });
  };

  // 自动贩卖机面板
  App.vendingTex = function (hot) {
    return App.canvasTex(256, 512, (ctx, w, h) => {
      ctx.fillStyle = hot ? '#f5ede0' : '#eef4fa'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = hot ? '#d8552e' : '#2e6bd8';
      ctx.fillRect(0, 0, w, 120);
      ctx.fillStyle = '#fff'; ctx.font = `900 54px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText(hot ? 'あたたか〜い' : 'つめた〜い', w / 2, 78);
      // 饮料排
      for (let r = 0; r < 4; r++) {
        for (let i = 0; i < 4; i++) {
          const colors = ['#e85a4f', '#f0c030', '#57b884', '#4a90d9', '#e07ab0'];
          ctx.fillStyle = colors[(r * 4 + i) % colors.length];
          ctx.fillRect(22 + i * 54, 150 + r * 62, 34, 44);
          ctx.fillStyle = 'rgba(255,255,255,0.5)';
          ctx.fillRect(22 + i * 54, 150 + r * 62, 34, 12);
        }
      }
      ctx.fillStyle = '#223'; ctx.fillRect(w - 70, h - 140, 46, 90); // 取物口
      ctx.fillStyle = hot ? '#a83a1e' : '#1e4aa8';
      ctx.fillRect(0, h - 34, w, 34);
    });
  };

  // 欢迎地垫
  App.matTex = function () {
    return App.canvasTex(256, 160, (ctx, w, h) => {
      ctx.fillStyle = '#3a5f4a'; ctx.fillRect(0, 0, w, h);
      ctx.strokeStyle = '#5a8f6f'; ctx.lineWidth = 8;
      ctx.strokeRect(12, 12, w - 24, h - 24);
      ctx.fillStyle = '#e8f0e0'; ctx.font = `bold 44px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText('いらっしゃいませ', w / 2, h / 2 + 16);
    });
  };

  // 路名牌
  App.streetSignTex = function () {
    return App.canvasTex(512, 160, (ctx, w, h) => {
      ctx.fillStyle = '#2f6f4f'; ctx.fillRect(0, 0, w, h);
      ctx.strokeStyle = '#fff'; ctx.lineWidth = 6; ctx.strokeRect(8, 8, w - 16, h - 16);
      ctx.fillStyle = '#fff'; ctx.textAlign = 'center';
      ctx.font = `900 66px ${FONT}`;
      ctx.fillText('ほしみどり', w * 0.32, h / 2 + 24);
      ctx.font = `900 66px ${FONT}`;
      ctx.fillText('2丁目', w * 0.75, h / 2 + 24);
      ctx.fillStyle = 'rgba(255,255,255,0.85)'; ctx.font = `bold 26px ${FONT}`;
      ctx.fillText('HOSHIMIDORI 2-CHOME', w / 2, h - 22);
    });
  };

  // 公告栏（海报栏）
  App.bulletinTex = function () {
    return App.canvasTex(512, 384, (ctx, w, h) => {
      ctx.fillStyle = '#e8e4d8'; ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = '#4a5468'; ctx.fillRect(0, 0, w, 46);
      ctx.fillStyle = '#fff'; ctx.font = `bold 30px ${FONT}`; ctx.textAlign = 'center';
      ctx.fillText('町内掲示板', w / 2, 34);
      // 三张小告示
      const cols = ['#f2d8c8', '#c8dcf2', '#e2f0c8'];
      for (let i = 0; i < 3; i++) {
        ctx.fillStyle = cols[i];
        ctx.fillRect(28 + i * 158, 72, 130, 190);
        ctx.fillStyle = '#5a5048'; ctx.font = `bold 22px ${FONT}`;
        ctx.fillText(['回収日', '夏祭り', '注意報'][i], 28 + i * 158 + 65, 110);
        for (let l = 0; l < 5; l++) {
          ctx.fillRect(40 + i * 158, 138 + l * 22, 104 - (l % 2) * 30, 7);
        }
      }
      ctx.fillStyle = '#5a5048'; ctx.font = `bold 24px ${FONT}`;
      ctx.fillText('よるマート × 町内会', w / 2, h - 46);
    });
  };

  // 竖向条纹光带（湿地反光 / 灯光池用）
  App.streakTex = function (tint) {
    return App.canvasTex(256, 256, (ctx, w, h) => {
      const g = ctx.createLinearGradient(0, 0, 0, h);
      g.addColorStop(0, tint); g.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = g; ctx.fillRect(0, 0, w, h);
      // 竖向亮纹（湿地水面拉丝感）
      for (let i = 0; i < 26; i++) {
        const x = Math.random() * w;
        const ww = 2 + Math.random() * 7;
        const a = 0.15 + Math.random() * 0.4;
        ctx.fillStyle = `rgba(255,255,255,${a})`;
        ctx.fillRect(x, 0, ww, h * (0.5 + Math.random() * 0.5));
      }
    });
  };

  // 径向光斑
  App.radialTex = function (inner, outer) {
    return App.canvasTex(256, 256, (ctx, w, h) => {
      const g = ctx.createRadialGradient(w / 2, h / 2, 4, w / 2, h / 2, w / 2);
      g.addColorStop(0, inner);
      g.addColorStop(1, outer);
      ctx.fillStyle = g; ctx.fillRect(0, 0, w, h);
    });
  };

  /* ---------- 地面贴片（灯光池 / 反光斑） ---------- */
  App.decal = function (w, d, tex, opt) {
    opt = opt || {};
    const m = new THREE.Mesh(
      new THREE.PlaneGeometry(w, d),
      new THREE.MeshBasicMaterial({
        map: tex,
        transparent: true,
        opacity: opt.opacity !== undefined ? opt.opacity : 0.5,
        blending: opt.blending || THREE.AdditiveBlending,
        depthWrite: false,
        side: THREE.DoubleSide,
      })
    );
    m.rotation.x = -Math.PI / 2;
    m.position.y = opt.y !== undefined ? opt.y : 0.012;
    m.renderOrder = 5;
    return m;
  };

})(window.App);
