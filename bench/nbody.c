// C reference for examples/nbody.hoshi (same algorithm, same structure).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { double x, y, z, vx, vy, vz, mass; } Body;

static void advance(Body *bodies, int n, double dt) {
    for (int i = 0; i < n; i++) {
        Body *bi = &bodies[i];
        for (int j = i + 1; j < n; j++) {
            Body *bj = &bodies[j];
            double dx = bi->x - bj->x, dy = bi->y - bj->y, dz = bi->z - bj->z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * sqrt(d2));
            bi->vx -= dx * bj->mass * mag;
            bi->vy -= dy * bj->mass * mag;
            bi->vz -= dz * bj->mass * mag;
            bj->vx += dx * bi->mass * mag;
            bj->vy += dy * bi->mass * mag;
            bj->vz += dz * bi->mass * mag;
        }
    }
    for (int i = 0; i < n; i++) {
        Body *b = &bodies[i];
        b->x += dt * b->vx;
        b->y += dt * b->vy;
        b->z += dt * b->vz;
    }
}

static double energy(Body *bodies, int n) {
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        Body *b = &bodies[i];
        e += 0.5 * b->mass * (b->vx * b->vx + b->vy * b->vy + b->vz * b->vz);
        for (int j = i + 1; j < n; j++) {
            Body *b2 = &bodies[j];
            double dx = b->x - b2->x, dy = b->y - b2->y, dz = b->z - b2->z;
            e -= b->mass * b2->mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void offset_momentum(Body *bodies, int n, double solar_mass) {
    double px = 0, py = 0, pz = 0;
    for (int i = 0; i < n; i++) {
        px += bodies[i].vx * bodies[i].mass;
        py += bodies[i].vy * bodies[i].mass;
        pz += bodies[i].vz * bodies[i].mass;
    }
    bodies[0].vx = -px / solar_mass;
    bodies[0].vy = -py / solar_mass;
    bodies[0].vz = -pz / solar_mass;
}

int main(int argc, char **argv) {
    int steps = argc > 1 ? atoi(argv[1]) : 1000;
    const double pi = 3.141592653589793, solar_mass = 4 * pi * pi, days = 365.24;
    Body bodies[5] = {
        {0, 0, 0, 0, 0, 0, solar_mass},
        {4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
         1.66007664274403694e-03 * days, 7.69901118419740425e-03 * days,
         -6.90460016972063023e-05 * days, 9.54791938424326609e-04 * solar_mass},
        {8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
         -2.76742510726862411e-03 * days, 4.99852801234917238e-03 * days,
         2.30417297573763929e-05 * days, 2.85885980666130812e-04 * solar_mass},
        {1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
         2.96460137564761618e-03 * days, 2.37847173959480950e-03 * days,
         -2.96589568540237556e-05 * days, 4.36624404335156298e-05 * solar_mass},
        {1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
         2.68067772490389322e-03 * days, 1.62824170038242295e-03 * days,
         -9.51592254519715870e-05 * days, 5.15138902046611451e-05 * solar_mass},
    };
    offset_momentum(bodies, 5, solar_mass);
    printf("%.9f\n", energy(bodies, 5));
    for (int s = 0; s < steps; s++)
        advance(bodies, 5, 0.01);
    printf("%.9f\n", energy(bodies, 5));
    return 0;
}
