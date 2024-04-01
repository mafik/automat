let canvas = document.getElementById("canvas");
let ctx = canvas.getContext("2d");
var width = window.innerWidth;
var height = window.innerHeight;


let dispenser = {
  x: round(canvas.width / 2),
  y: round(canvas.height / 4),
  a: 0,
  v: 0,
  dir: 3 * Math.PI / 2,
};

function OnResize() {
  width = window.innerWidth;
  height = window.innerHeight;
  let density = window.devicePixelRatio || 1;
  canvas.width = window.innerWidth * density;
  canvas.height = window.innerHeight * density;
  ctx = canvas.getContext("2d");
  ctx.font = "20px Iosevka";
  ctx.scale(density, density);
  dispenser.x = round(width / 2);
  dispenser.y = round(height / 4);
}

window.addEventListener("resize", OnResize);
OnResize();

function round(x) {
  return Math.round(x);
}

let mouse = {
  x: round(canvas.width / 4),
  y: round(canvas.height / 2),
};
let mouse_down = true;

window.addEventListener("mousemove", function (e) {
  // Check if LMB is down
  if (!e.buttons & 1) return;
  mouse.x = e.clientX;
  mouse.y = e.clientY;
});

window.addEventListener("mousedown", function (e) {
  mouse.x = e.clientX;
  mouse.y = e.clientY;
  mouse_down = true;
});

window.addEventListener("mouseup", function (e) {
  mouse.x = dispenser.x;
  mouse.y = dispenser.y;
  mouse_down = false;
});

window.addEventListener("touchmove", function (e) {
  e.preventDefault();
  mouse.x = e.touches[0].clientX;
  mouse.y = e.touches[0].clientY;
});

window.addEventListener("touchstart", function (e) {
  e.preventDefault();
  mouse.x = e.touches[0].clientX;
  mouse.y = e.touches[0].clientY;
  mouse_down = true;
});

window.addEventListener("touchend", function (e) {
  e.preventDefault();
  mouse.x = dispenser.x;
  mouse.y = dispenser.y;
  mouse_down = false;
});

class ChainLink {
  constructor() {
    this.x = dispenser.x;
    this.y = dispenser.y;
    this.vx = 0;
    this.vy = 0;
    this.ax = 0;
    this.ay = 0;
  }
};

let chain = [
  new ChainLink(),
];

var last_t = 0;
function AnimationFrame(t) {

  ctx.clearRect(0, 0, canvas.width, canvas.height);

  ctx.fillStyle = "#000";
  ctx.fillText('Δt = ' + (t - last_t).toFixed(2), 10, 30);

  let dt = t - last_t;
  last_t = t;
  dt /= 1000;
  if (dt > 0.1) {
    dt = 0.1;
  }
  if (dt < 0.001) {
    dt = 0.001;
  }

  ctx.strokeStyle = "red";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.rect(dispenser.x - 10, dispenser.y - 10, 20, 10);
  ctx.stroke();


  let targets = [
    { x: mouse.x, y: mouse.y },
    { x: mouse.x, y: round((mouse.y + dispenser.y) / 2) },
    { x: dispenser.x, y: round((mouse.y + dispenser.y) / 2) },
    { x: dispenser.x, y: dispenser.y },
  ];

  // Draw target line
  ctx.strokeStyle = "rgba(0, 0, 0, 0.1)";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(targets[0].x, targets[0].y);
  for (let t of targets) {
    ctx.lineTo(t.x, t.y);
  }
  ctx.stroke();

  // Find focus points along the target line
  let focus_points = [];
  if (mouse_down) {
    focus_points.push({ x: mouse.x, y: mouse.y });
  }
  const step = 20;
  {
    let it = {
      x: targets[0].x,
      y: targets[0].y
    };
    let next_target = 1;
    let remaining = step;
    while (next_target < targets.length) {
      let next = targets[next_target];
      let dx = next.x - it.x;
      let dy = next.y - it.y;
      let dist = Math.hypot(dx, dy);
      if (dist < remaining) {
        remaining -= dist;
        it.x = next.x;
        it.y = next.y;
        next_target++;
      } else {
        it.x += dx / dist * remaining;
        it.y += dy / dist * remaining;
        focus_points.push({ x: it.x, y: it.y });
        remaining = step;
      }
    }
  }

  // Draw focus points
  ctx.strokeStyle = "#f80";
  ctx.lineWidth = 1;
  for (let f of focus_points) {
    ctx.beginPath();
    const cross_size = 3;
    ctx.moveTo(f.x - cross_size, f.y - cross_size);
    ctx.lineTo(f.x + cross_size, f.y + cross_size);
    ctx.moveTo(f.x - cross_size, f.y + cross_size);
    ctx.lineTo(f.x + cross_size, f.y - cross_size);
    ctx.stroke();
  }

  // Zero forces
  for (let c of chain) {
    c.ax = 0;
    c.ay = 0;
  }

  // Apply spring force pulling each chain link to its focus point
  for (let i = 0; i < chain.length && i < focus_points.length; i++) {
    let c = chain[i];
    let target = focus_points[i];
    let dx = target.x - c.x;
    let dy = target.y - c.y;
    c.ax += dx * 1e2;
    c.ay += dy * 1e2;
  }

  // Apply spring force pulling each chain link towards the control points of its neighbors
  const stiffness = 1e3;
  ctx.fillStyle = "#f00";
  for (let i = 1; i < chain.length; i++) {
    let c = chain[i];
    let prev = chain[i - 1];
    if ('dir' in prev) {
      let fx = (prev.x + Math.cos(prev.dir) * step - c.x) * stiffness;
      let fy = (prev.y + Math.sin(prev.dir) * step - c.y) * stiffness;
      c.ax += fx;
      c.ay += fy;
      prev.ax -= fx;
      prev.ay -= fy;
      ctx.beginPath();
      ctx.arc(prev.x + Math.cos(prev.dir) * step, prev.y + Math.sin(prev.dir) * step, 1, 0, 2 * Math.PI);
      ctx.fill();
    }
  }
  for (let i = 0; i < chain.length; i++) {
    let c = chain[i];
    let next;
    if (i < chain.length - 1) {
      next = chain[i + 1];
    } else {
      next = dispenser;
    }
    if ('dir' in next) {
      let fx = (next.x - Math.cos(next.dir) * step - c.x) * stiffness;
      let fy = (next.y - Math.sin(next.dir) * step - c.y) * stiffness;
      c.ax += fx;
      c.ay += fy;
      next.ax -= fx;
      next.ay -= fy;
      ctx.beginPath();
      ctx.arc(next.x - Math.cos(next.dir) * step, next.y - Math.sin(next.dir) * step, 1, 0, 2 * Math.PI);
      ctx.fill();
    }
  }

  for (let c of chain) {
    c.vx += c.ax * dt;
    c.vy += c.ay * dt;
  }

  for (let i = 0; i < chain.length; i++) {
    let c = chain[i];
    if (i < focus_points.length) {
      c.vx *= Math.exp(-20 * dt);
      c.vy *= Math.exp(-20 * dt);
    } else {
      c.vx *= Math.exp(-2 * dt);
      c.vy *= Math.exp(-2 * dt);
    }
    let dist0 = Math.hypot(c.x - dispenser.x, c.y - dispenser.y);
    c.x += c.vx * dt;
    c.y += c.vy * dt;
    if (i == 0 && focus_points.length > 0) {
      c.x = focus_points[0].x;
      c.y = focus_points[0].y;
    }
    // If we're too far from the dispenser, add a new link
    if (i == chain.length - 1) {
      let dx = c.x - dispenser.x;
      let dy = c.y - dispenser.y;
      let dist = Math.hypot(dx, dy);
      if (dist > step + dispenser.v * dt * 2) {
        chain.push(new ChainLink());
        let last = chain[chain.length - 1];
        last.x = c.x - dx / dist * step;
        last.y = c.y - dy / dist * step;
        dispenser.v = 0;
      }
    }
    if (chain.length > focus_points.length && i == chain.length - 1) {
      // Dispenser is retracting the cable. Enforce distance constraint
      let dx1 = c.x - dispenser.x;
      let dy1 = c.y - dispenser.y;
      let dist1 = Math.hypot(dx1, dy1);
      if (dist1 > dist0) {
        let new_x = dispenser.x + dx1 / dist1 * dist0;
        let new_y = dispenser.y + dy1 / dist1 * dist0;
        c.vx += (new_x - c.x) / dt;
        c.vy += (new_y - c.y) / dt;
        c.x = new_x;
        c.y = new_y;
      }
    }
  }

  // Dispenser pulling the chain in
  if (chain.length > focus_points.length) {
    dispenser.a = 5e2;
    dispenser.v += dispenser.a * dt;
    dispenser.v *= Math.exp(-1 * dt); // Limit the maximum speed
    let retract = dispenser.v * dt;
    // Shorten the final link by pulling it towards the dispenser
    let prev = dispenser;
    let total_dist = 0;
    for (let i = chain.length - 1; i >= 1; i--) {
      let curr = chain[i];
      let dist = Math.hypot(curr.x - prev.x, curr.y - prev.y);
      total_dist += dist;
      if (total_dist > retract) {
        let new_dist = total_dist - retract;
        let dx = curr.x - dispenser.x;
        let dy = curr.y - dispenser.y;
        let dist = Math.hypot(dx, dy);
        if (new_dist < dist) {
          let new_x = dispenser.x + dx / dist * new_dist;
          let new_y = dispenser.y + dy / dist * new_dist;
          curr.vx += (new_x - curr.x) / dt;
          curr.vy += (new_y - curr.y) / dt;
          curr.x = new_x;
          curr.y = new_y;
          // Make sure that the speed component in the direction of dispenser is set to dispenser.v
          let speed_along_dist = (curr.vx * dx + curr.vy * dy) / dist;
          curr.vx -= (dispenser.v + speed_along_dist) * dx / dist;
          curr.vy -= (dispenser.v + speed_along_dist) * dy / dist;
        }
        break;
      } else {
        prev = curr;
        chain.pop();
      }
    }
    retract -= total_dist;
    if (retract < 0) retract = 0;
    if (chain.length == 1 && retract > 0) {
      let c = chain[0];
      let dx = c.x - dispenser.x;
      let dy = c.y - dispenser.y;
      let dist = Math.hypot(dx, dy);
      if (dist < 0.1) dist = 0.1;
      let new_dist = dist - retract;
      if (new_dist < 0) new_dist = 0;
      let new_x = dispenser.x + dx / dist * new_dist;
      let new_y = dispenser.y + dy / dist * new_dist;
      c.vx += (new_x - c.x) / dt;
      c.vy += (new_y - c.y) / dt;
      c.x = new_x;
      c.y = new_y;
    }
  } else {
    dispenser.a = 0;
    dispenser.v = 0;
  }

  // Adjust chain lengths
  for (let i = 1; i < chain.length; i++) {
    let curr = chain[i];
    let prev = chain[i - 1];
    let cx = (curr.x + prev.x) / 2;
    let cy = (curr.y + prev.y) / 2;
    let dx = curr.x - prev.x;
    let dy = curr.y - prev.y;
    let dist = Math.hypot(dx, dy);
    let new_curr_x = cx + dx / dist * step / 2;
    let new_curr_y = cy + dy / dist * step / 2;
    let new_prev_x = cx - dx / dist * step / 2;
    let new_prev_y = cy - dy / dist * step / 2;
    curr.vx += (new_curr_x - curr.x) / dt;
    curr.vy += (new_curr_y - curr.y) / dt;
    prev.vx += (new_prev_x - prev.x) / dt;
    prev.vy += (new_prev_y - prev.y) / dt;
    curr.x = new_curr_x;
    curr.y = new_curr_y;
    prev.x = new_prev_x;
    prev.y = new_prev_y;
  }


  // Find control points
  for (let i = 1; i < chain.length; i++) {
    let prev = chain[i - 1];
    let curr = chain[i];
    let next = i < chain.length - 1 ? chain[i + 1] : dispenser;
    let dx = next.x - prev.x;
    let dy = next.y - prev.y;
    let dist = Math.hypot(dx, dy);
    dx = dx / dist * step / 3;
    dy = dy / dist * step / 3;
    curr.cp1x = curr.x - dx;
    curr.cp1y = curr.y - dy;
    curr.cp2x = curr.x + dx;
    curr.cp2y = curr.y + dy;
  }
  dispenser.cp1x = dispenser.x;
  dispenser.cp1y = dispenser.y + step / 4;

  if (focus_points.length > 0) {
    chain[0].cp2x = chain[0].x;
    chain[0].cp2y = chain[0].y - step / 4;
  } else {
    if (chain.length >= 2) {
      chain[0].cp2x = chain[1].cp1x;
      chain[0].cp2y = chain[1].cp1y;
    } else {
      chain[0].cp2x = chain[0].x;
      chain[0].cp2y = chain[0].y - step / 4;
    }
  }
  for (let i = 0; i < chain.length; i++) {
    let p = chain[i];
    p.dir = Math.atan2(p.cp2y - p.y, p.cp2x - p.x);
  }

  // Draw chain links
  ctx.fillStyle = "#08f";
  ctx.strokeStyle = "#888";
  for (let c of chain) {
    ctx.beginPath();
    ctx.arc(c.x, c.y, 3, 0, 2 * Math.PI);
    ctx.fill();

    ctx.beginPath();
    ctx.moveTo(c.x, c.y);
    ctx.lineTo(c.cp1x, c.cp1y);
    ctx.stroke();

    ctx.beginPath();
    ctx.moveTo(c.x, c.y);
    ctx.lineTo(c.cp2x, c.cp2y);
    ctx.stroke();
  }

  // Draw chain as a smooth curve
  ctx.strokeStyle = "#08f";
  ctx.lineWidth = 1;
  ctx.beginPath();

  ctx.moveTo(chain[0].x, chain[0].y);
  for (let i = 1; i < chain.length; i++) {
    let prev = chain[i - 1];
    let curr = chain[i];
    ctx.bezierCurveTo(prev.cp2x, prev.cp2y, curr.cp1x, curr.cp1y, curr.x, curr.y);
  }
  ctx.bezierCurveTo(chain[chain.length - 1].cp2x, chain[chain.length - 1].cp2y, dispenser.cp1x, dispenser.cp1y, dispenser.x, dispenser.y);

  ctx.stroke();

  ctx.save();
  ctx.translate(chain[0].x, chain[0].y);
  let alpha = Math.atan2(chain[0].cp2y - chain[0].y, chain[0].cp2x - chain[0].x) + Math.PI / 2;
  ctx.rotate(alpha);
  // Draw the plug at the end of the chain
  ctx.beginPath();
  ctx.moveTo(-10, 0);
  ctx.lineTo(10, 0);
  ctx.lineTo(10, 20);
  ctx.lineTo(-10, 20);
  ctx.closePath();
  ctx.stroke();
  ctx.restore();

  requestAnimationFrame(AnimationFrame);
}

AnimationFrame(0);
