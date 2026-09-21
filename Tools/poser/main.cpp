// SandiumPoser — standalone Sub Rosa player-model pose editor.
//
// Renders the real skinned playermodel (data/model/*.cmc: 16 parts, per-vertex
// bone offsets + weights) and an attached item mesh (.cmo, e.g. the custom
// revolver). Per-joint euler posing, item grip transform, text presets under
// Pose/*.preset. The "9mm" preset is captured from the running game by reading
// Human.bones[16] (position + orientation) with a 9mm equipped.
//
// Build: cmake --build Suitium/Output --config Release --target SandiumPoser

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <GL/gl.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comdlg32.lib")
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- math
struct Vec3 { float x, y, z; };
struct Mat4 { float m[16] = {}; }; // column-major

static Mat4 Identity()
{
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
static Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
static Mat4 Translate(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}
static Mat4 RotX(float a)
{
    Mat4 r = Identity(); float c = cosf(a), s = sinf(a);
    r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
    return r;
}
static Mat4 RotY(float a)
{
    Mat4 r = Identity(); float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}
static Mat4 RotZ(float a)
{
    Mat4 r = Identity(); float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}
static Vec3 TransformPoint(const Mat4 &m, Vec3 p)
{
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}
static Mat4 InverseRigid(const Mat4 &m)
{
    Mat4 r = Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 4 + j] = m.m[j * 4 + i];
    float x = m.m[12], y = m.m[13], z = m.m[14];
    r.m[12] = -(r.m[0] * x + r.m[4] * y + r.m[8] * z);
    r.m[13] = -(r.m[1] * x + r.m[5] * y + r.m[9] * z);
    r.m[14] = -(r.m[2] * x + r.m[6] * y + r.m[10] * z);
    return r;
}

// ---------------------------------------------------------------- data
struct SkinVertex
{
    float pos[3];
    float uv[2];
    float rawOffset[16][3]; // bone-local position (file, unscaled)
    int bone[4];
    float weight[4];
    int count;
};
struct CmcModel
{
    float pivot[16][3]; // scaled local joint offsets
    std::vector<SkinVertex> verts;
    std::vector<uint32_t> indices;
    Mat4 bind[16];
    Mat4 bindInv[16];
    bool bindOk[16];
};
struct CmoModel
{
    std::vector<float> pos;
    std::vector<uint32_t> indices;
};

static bool LoadCmc(const char *path, CmcModel &out)
{
    FILE *f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    if (size < 0x100 || memcmp(data.data(), "CMod", 4) != 0)
        return false;
    uint32_t version = 0, partCount = 0;
    memcpy(&version, &data[4], 4);
    memcpy(&partCount, &data[8], 4);
    if (version != 2 || partCount != 16)
        return false;

    const float scale = 1.125f;
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k)
        {
            float v;
            memcpy(&v, &data[12 + i * 12 + k * 4], 4);
            out.pivot[i][k] = v * scale;
        }
    uint32_t vertexCount = 0;
    memcpy(&vertexCount, &data[0xCC], 4);
    size_t base = 0xD0;
    out.verts.resize(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v)
    {
        const uint8_t *rec = &data[base + v * 276];
        SkinVertex &sv = out.verts[v];
        memcpy(sv.pos, rec, 12);
        memcpy(sv.uv, rec + 268, 8);
        sv.count = 0;
        float total = 0.0f;
        for (int b = 0; b < 16; ++b)
        {
            const uint8_t *e = rec + 12 + b * 16;
            float ox, oy, oz, w;
            memcpy(&ox, e, 4);
            memcpy(&oy, e + 4, 4);
            memcpy(&oz, e + 8, 4);
            memcpy(&w, e + 12, 4);
            sv.rawOffset[b][0] = ox * scale;
            sv.rawOffset[b][1] = oy * scale;
            sv.rawOffset[b][2] = oz * scale;
            if (w > 1e-9f && sv.count < 4)
            {
                sv.bone[sv.count] = b;
                sv.weight[sv.count] = w;
                sv.count++;
                total += w;
            }
        }
        if (total > 0.0f)
            for (int k = 0; k < sv.count; ++k)
                sv.weight[k] /= total;
    }
    size_t foff = base + (size_t)vertexCount * 276;
    uint32_t faceCount = 0;
    memcpy(&faceCount, &data[foff], 4);
    out.indices.resize((size_t)faceCount * 3);
    memcpy(out.indices.data(), &data[foff + 4], (size_t)faceCount * 12);
    return true;
}

static bool LoadCmo(const char *path, CmoModel &out)
{
    FILE *f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    if (size < 12 || memcmp(data.data(), "CMod", 4) != 0)
        return false;
    uint32_t version = 0, vertexCount = 0;
    memcpy(&version, &data[4], 4);
    memcpy(&vertexCount, &data[8], 4);
    if (version < 1 || version > 3)
        return false;
    size_t off = 12;
    out.pos.resize((size_t)vertexCount * 3);
    for (uint32_t v = 0; v < vertexCount; ++v)
    {
        memcpy(&out.pos[v * 3], &data[off], 12);
        off += 24;
    }
    uint32_t faceCount = 0;
    memcpy(&faceCount, &data[off], 4);
    off += 4;
    out.indices.clear();
    for (uint32_t fc = 0; fc < faceCount; ++fc)
    {
        uint32_t count = 0;
        memcpy(&count, &data[off], 4);
        off += 4;
        for (uint32_t k = 0; k < count; ++k)
        {
            uint32_t idx = 0;
            memcpy(&idx, &data[off], 4);
            off += 4;
            if (k < 3)
                out.indices.push_back(idx);
        }
        off += 8;
    }
    return true;
}

// ---------------------------------------------------------------- skeleton
static const int kParent[16] = {-1, 0, 1, 1, 1, 4, 5, 1, 7, 8, 0, 10, 11, 0, 13, 14};
static const char *kBoneNames[16] = {
    "pelvis", "chest", "head", "face", "left arm", "left forearm", "left hand",
    "right arm", "right forearm", "right hand", "left thigh", "left shin",
    "left foot", "right thigh", "right shin", "right foot"};

// Kabsch rigid fit: R,t minimizing |R*p + t - q|
static Mat4 Kabsch(const std::vector<Vec3> &P, const std::vector<Vec3> &Q)
{
    size_t n = P.size();
    Vec3 cp{0, 0, 0}, cq{0, 0, 0};
    for (size_t i = 0; i < n; ++i)
    {
        cp.x += P[i].x / n; cp.y += P[i].y / n; cp.z += P[i].z / n;
        cq.x += Q[i].x / n; cq.y += Q[i].y / n; cq.z += Q[i].z / n;
    }
    float H[3][3] = {};
    for (size_t i = 0; i < n; ++i)
    {
        float p[3] = {P[i].x - cp.x, P[i].y - cp.y, P[i].z - cp.z};
        float q[3] = {Q[i].x - cq.x, Q[i].y - cq.y, Q[i].z - cq.z};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                H[r][c] += p[r] * q[c];
    }
    auto eig = [](float M[3][3], float V[3][3]) {
        float A[3][3];
        memcpy(A, M, sizeof(A));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                V[i][j] = (i == j) ? 1.0f : 0.0f;
        for (int sweep = 0; sweep < 60; ++sweep)
        {
            int pi = 0, pj = 1;
            float big = 0.0f;
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j)
                    if (fabsf(A[i][j]) > big) { big = fabsf(A[i][j]); pi = i; pj = j; }
            if (big < 1e-9f)
                break;
            float theta = 0.5f * atan2f(2.0f * A[pi][pj], A[pj][pj] - A[pi][pi]);
            float c = cosf(theta), s = sinf(theta);
            for (int k = 0; k < 3; ++k)
            {
                float mik = A[k][pi], mkj = A[k][pj];
                A[k][pi] = c * mik - s * mkj;
                A[k][pj] = s * mik + c * mkj;
            }
            for (int k = 0; k < 3; ++k)
            {
                float mki = A[pi][k], mkj = A[pj][k];
                A[pi][k] = c * mki - s * mkj;
                A[pj][k] = s * mki + c * mkj;
            }
            for (int k = 0; k < 3; ++k)
            {
                float vki = V[k][pi], vkj = V[k][pj];
                V[k][pi] = c * vki - s * vkj;
                V[k][pj] = s * vki + c * vkj;
            }
        }
    };
    float HtH[3][3] = {}, HHt[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            HtH[i][j] = H[0][i] * H[0][j] + H[1][i] * H[1][j] + H[2][i] * H[2][j];
            HHt[i][j] = H[i][0] * H[j][0] + H[i][1] * H[j][1] + H[i][2] * H[j][2];
        }
    float U[3][3], V[3][3];
    eig(HHt, U);
    eig(HtH, V);
    Mat4 best = Identity();
    float bestErr = 1e30f;
    for (int combo = 0; combo < 8; ++combo)
    {
        float su[3] = {(combo & 1) ? -1.0f : 1.0f, (combo & 2) ? -1.0f : 1.0f,
                       (combo & 4) ? -1.0f : 1.0f};
        Mat4 R = Identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            {
                float s = 0.0f;
                for (int k = 0; k < 3; ++k)
                    s += V[r][k] * su[k] * U[c][k];
                R.m[c * 4 + r] = s;
            }
        float det = R.m[0] * (R.m[5] * R.m[10] - R.m[6] * R.m[9]) -
                    R.m[4] * (R.m[1] * R.m[10] - R.m[2] * R.m[9]) +
                    R.m[8] * (R.m[1] * R.m[6] - R.m[2] * R.m[5]);
        if (det < 0.0f)
            continue;
        float err = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            Vec3 rp = TransformPoint(R, P[i]);
            err += (rp.x - Q[i].x) * (rp.x - Q[i].x) + (rp.y - Q[i].y) * (rp.y - Q[i].y) +
                   (rp.z - Q[i].z) * (rp.z - Q[i].z);
        }
        if (err < bestErr) { bestErr = err; best = R; }
    }
    Mat4 result = Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.m[c * 4 + r] = best.m[c * 4 + r];
    result.m[12] = cq.x - (best.m[0] * cp.x + best.m[4] * cp.y + best.m[8] * cp.z);
    result.m[13] = cq.y - (best.m[1] * cp.x + best.m[5] * cp.y + best.m[9] * cp.z);
    result.m[14] = cq.z - (best.m[2] * cp.x + best.m[6] * cp.y + best.m[10] * cp.z);
    return result;
}

struct PoseState
{
    float euler[16][3] = {}; // radians
    Vec3 itemOffset{0.02f, 0.0f, 0.0f};
    Vec3 itemRotation{0.0f, 0.0f, 0.0f};
    int itemBone = 9;
    char itemPath[MAX_PATH] = "sandium/models/revolver/revolver.cmo";
} g_pose;

static CmcModel g_body;
static CmoModel g_item;
static bool g_hasBody = false, g_hasItem = false;
static Mat4 g_localBind[16], g_bindInv[16], g_world[16];
static bool g_bindOk[16];

static void SolveBinds(const char *cmcPath)
{
    // fit each bone's bind matrix from single-weight vertices: offset -> file pos
    std::vector<Vec3> P[16], Q[16];
    for (const SkinVertex &sv : g_body.verts)
    {
        if (sv.count != 1)
            continue;
        Vec3 off = {sv.rawOffset[sv.bone[0]][0], sv.rawOffset[sv.bone[0]][1],
                    sv.rawOffset[sv.bone[0]][2]};
        P[sv.bone[0]].push_back(off);
        Q[sv.bone[0]].push_back({sv.pos[0], sv.pos[1], sv.pos[2]});
    }
    // hierarchy order so fallbacks can use the parent's solved world
    Mat4 fallback = Identity();
    for (int pass = 0; pass < 4; ++pass)
    {
        for (int b = 0; b < 16; ++b)
        {
            if (g_bindOk[b])
                continue;
            if (P[b].size() >= 10)
            {
                g_body.bind[b] = Kabsch(P[b], Q[b]);
                g_bindOk[b] = true;
            }
            else if (pass > 0)
            {
                int parent = kParent[b];
                Mat4 parentWorld = (parent < 0) ? Identity() : g_body.bind[parent];
                bool parentOk = (parent < 0) ? true : g_bindOk[parent];
                if (!parentOk)
                    continue;
                Vec3 pivot = {g_body.pivot[b][0], g_body.pivot[b][1], g_body.pivot[b][2]};
                Vec3 t = TransformPoint(parentWorld, pivot);
                g_body.bind[b] = Translate(t.x, t.y, t.z);
                g_bindOk[b] = true;
            }
        }
    }
    for (int b = 0; b < 16; ++b)
    {
        g_bindInv[b] = InverseRigid(g_body.bind[b]);
        g_localBind[b] = (kParent[b] < 0) ? g_body.bind[b]
                                          : InverseRigid(g_body.bind[kParent[b]]) * g_body.bind[b];
    }
    (void)cmcPath;
}

// ---------------------------------------------------------------- app state
static HWND g_window = nullptr, g_panel = nullptr, g_glChild = nullptr;
static HDC g_dc = nullptr;
static HGLRC g_rc = nullptr;
static int g_winW = 1280, g_winH = 800;
static const int kPanelW = 380;
static float g_camYaw = 0.6f, g_camPitch = 0.25f, g_camDist = 3.2f;
static Vec3 g_camTarget{0.0f, 0.95f, 0.0f};
static int g_selectedBone = 9;
static int g_sliderValues[3] = {0, 0, 0}; // degrees for the selected bone
static std::vector<std::string> g_presets;
static char g_status[256] = "";

enum
{
    IDC_LIST_BONES = 100,
    IDC_SLIDER_X, IDC_SLIDER_Y, IDC_SLIDER_Z,
    IDC_VAL_X, IDC_VAL_Y, IDC_VAL_Z,
    IDC_BTN_ZERO, IDC_BTN_ZEROALL,
    IDC_COMBO_ATTACH, IDC_ITEM_X, IDC_ITEM_Y, IDC_ITEM_Z,
    IDC_ITEM_RX, IDC_ITEM_RY, IDC_ITEM_RZ,
    IDC_EDIT_ITEM, IDC_BTN_BROWSE,
    IDC_COMBO_PRESET, IDC_BTN_SAVE, IDC_BTN_LOAD,
    IDC_STATIC_STATUS
};

static std::string g_exeDir;

static std::string PresetDir() { return g_exeDir + "Pose"; }

static void RefreshPresets(HWND combo)
{
    SendMessage(combo, CB_RESETCONTENT, 0, 0);
    g_presets.clear();
    WIN32_FIND_DATAA fd;
    std::string pattern = PresetDir() + "\\*.preset";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            g_presets.push_back(fd.cFileName);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void UpdateSlidersFromState()
{
    for (int a = 0; a < 3; ++a)
        g_sliderValues[a] = (int)(g_pose.euler[g_selectedBone][a] * 180.0f / 3.14159265f);
    SendMessage(GetDlgItem(g_panel, IDC_SLIDER_X), TBM_SETPOS, TRUE, g_sliderValues[0] + 180);
    SendMessage(GetDlgItem(g_panel, IDC_SLIDER_Y), TBM_SETPOS, TRUE, g_sliderValues[1] + 180);
    SendMessage(GetDlgItem(g_panel, IDC_SLIDER_Z), TBM_SETPOS, TRUE, g_sliderValues[2] + 180);
    char buf[32];
    for (int a = 0; a < 3; ++a)
    {
        sprintf_s(buf, "%d", g_sliderValues[a]);
        SetDlgItemTextA(g_panel, IDC_VAL_X + a, buf);
    }
    SendDlgItemMessageA(g_panel, IDC_COMBO_ATTACH, CB_SETCURSEL, g_pose.itemBone, 0);
}

static void SavePreset(const char *path)
{
    FILE *f = nullptr;
    fopen_s(&f, path, "w");
    if (!f)
        return;
    fprintf(f, "sandiumpose 1\n");
    for (int b = 0; b < 16; ++b)
        fprintf(f, "euler %d %.4f %.4f %.4f\n", b, g_pose.euler[b][0], g_pose.euler[b][1],
                g_pose.euler[b][2]);
    fprintf(f, "attach %d %.4f %.4f %.4f %.4f %.4f %.4f\n", g_pose.itemBone,
            g_pose.itemOffset.x, g_pose.itemOffset.y, g_pose.itemOffset.z,
            g_pose.itemRotation.x, g_pose.itemRotation.y, g_pose.itemRotation.z);
    fprintf(f, "item %s\n", g_pose.itemPath);
    fclose(f);
}

static bool LoadPresetFile(const char *path)
{
    FILE *f = nullptr;
    fopen_s(&f, path, "r");
    if (!f)
        return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "euler ", 6) == 0)
        {
            int b = 0;
            float x, y, z;
            if (sscanf_s(line + 6, "%d %f %f %f", &b, &x, &y, &z) == 4 && b >= 0 && b < 16)
            {
                g_pose.euler[b][0] = x;
                g_pose.euler[b][1] = y;
                g_pose.euler[b][2] = z;
                ok = true;
            }
        }
        else if (strncmp(line, "attach ", 7) == 0)
        {
            int b = 0;
            float v[6];
            if (sscanf_s(line + 7, "%d %f %f %f %f %f %f", &b, &v[0], &v[1], &v[2], &v[3],
                         &v[4], &v[5]) == 7 && b >= 0 && b < 16)
            {
                g_pose.itemBone = b;
                g_pose.itemOffset = {v[0], v[1], v[2]};
                g_pose.itemRotation = {v[3], v[4], v[5]};
                ok = true;
            }
        }
        else if (strncmp(line, "item ", 5) == 0)
        {
            char *p = line + 5, *e = line + strlen(line);
            while (e > p && (e[-1] == '\n' || e[-1] == '\r'))
                --e;
            *e = 0;
            strncpy_s(g_pose.itemPath, p, _TRUNCATE);
        }
    }
    fclose(f);
    return ok;
}

static void ApplySliders()
{
    for (int a = 0; a < 3; ++a)
        g_pose.euler[g_selectedBone][a] = g_sliderValues[a] * 3.14159265f / 180.0f;
}

static void ComputeWorld()
{
    for (int b = 0; b < 16; ++b)
    {
        Mat4 userR = RotZ(g_pose.euler[b][2]) * RotY(g_pose.euler[b][1]) * RotX(g_pose.euler[b][0]);
        Mat4 local = userR * g_localBind[b];
        g_world[b] = (kParent[b] < 0) ? local : g_world[kParent[b]] * local;
    }
}

// ---------------------------------------------------------------- GL
static void DrawCmo(const CmoModel &m, float r, float g, float b)
{
    glBegin(GL_TRIANGLES);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        uint32_t ia = m.indices[i], ib = m.indices[i + 1], ic = m.indices[i + 2];
        const float *A = &m.pos[ia * 3], *B = &m.pos[ib * 3], *C = &m.pos[ic * 3];
        float ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2];
        float vx = C[0] - A[0], vy = C[1] - A[1], vz = C[2] - A[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-9f;
        nx /= len; ny /= len; nz /= len;
        glNormal3f(nx, ny, nz);
        glVertex3f(A[0], A[1], A[2]);
        glVertex3f(B[0], B[1], B[2]);
        glVertex3f(C[0], C[1], C[2]);
    }
    glEnd();
    (void)r; (void)g; (void)b;
}

static void gluPerspectiveHolder(double fovy, double aspect, double zNear, double zFar);

static void Render()
{
    int vpW = g_winW - kPanelW, vpH = g_winH;
    glViewport(0, 0, vpW, vpH);
    glClearColor(0.32f, 0.36f, 0.42f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = vpH > 0 ? (float)vpW / vpH : 1.0f;
    gluPerspectiveHolder(50.0, aspect, 0.05, 50.0);

    Mat4 view = Translate(0.0f, 0.0f, -g_camDist) * RotX(-g_camPitch) * RotY(-g_camYaw) *
                Translate(-g_camTarget.x, -g_camTarget.y, -g_camTarget.z);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    GLfloat lightPos[] = {0.4f, 1.0f, 0.6f, 0.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    GLfloat diffuse[] = {0.9f, 0.9f, 0.9f, 1.0f};
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    GLfloat ambient[] = {0.45f, 0.45f, 0.5f, 1.0f};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);

    // grid
    glDisable(GL_LIGHTING);
    glColor3f(0.45f, 0.47f, 0.5f);
    glBegin(GL_LINES);
    for (int i = -5; i <= 5; ++i)
    {
        glVertex3f((float)i * 0.5f, 0.0f, -2.5f);
        glVertex3f((float)i * 0.5f, 0.0f, 2.5f);
        glVertex3f(-2.5f, 0.0f, (float)i * 0.5f);
        glVertex3f(2.5f, 0.0f, (float)i * 0.5f);
    }
    glEnd();
    glEnable(GL_LIGHTING);

    if (!g_hasBody)
        return;

    ComputeWorld();
    std::vector<Vec3> skinned(g_body.verts.size());
    for (size_t v = 0; v < g_body.verts.size(); ++v)
    {
        const SkinVertex &sv = g_body.verts[v];
        Vec3 p{0, 0, 0};
        Vec3 filePos = {sv.pos[0], sv.pos[1], sv.pos[2]};
        for (int k = 0; k < sv.count; ++k)
        {
            Mat4 m = g_world[sv.bone[k]] * g_bindInv[sv.bone[k]];
            Vec3 t = TransformPoint(m, filePos);
            p.x += sv.weight[k] * t.x;
            p.y += sv.weight[k] * t.y;
            p.z += sv.weight[k] * t.z;
        }
        skinned[v] = p;
    }

    glBegin(GL_TRIANGLES);
    for (size_t i = 0; i + 2 < g_body.indices.size(); i += 3)
    {
        uint32_t ia = g_body.indices[i], ib = g_body.indices[i + 1], ic = g_body.indices[i + 2];
        Vec3 A = skinned[ia], B = skinned[ib], C = skinned[ic];
        float ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
        float vx = C.x - A.x, vy = C.y - A.y, vz = C.z - A.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-9f;
        // subtle per-part tint so joints read clearly
        int dominant = g_body.verts[ia].count ? g_body.verts[ia].bone[0] : 0;
        float tint = 0.85f + 0.03f * (dominant % 4);
        glColor3f(0.72f * tint, 0.74f * tint, 0.78f * tint);
        glNormal3f(nx / len, ny / len, nz / len);
        glVertex3f(A.x, A.y, A.z);
        glVertex3f(B.x, B.y, B.z);
        glVertex3f(C.x, C.y, C.z);
    }
    glEnd();

    // joints
    glDisable(GL_LIGHTING);
    glPointSize(6.0f);
    glBegin(GL_POINTS);
    for (int b = 0; b < 16; ++b)
    {
        Vec3 j = TransformPoint(g_world[b], {0, 0, 0});
        if (b == g_selectedBone)
            glColor3f(1.0f, 0.3f, 0.2f);
        else
            glColor3f(0.15f, 0.8f, 0.3f);
        glVertex3f(j.x, j.y, j.z);
    }
    glEnd();
    glEnable(GL_LIGHTING);

    // attached item
    if (g_hasItem)
    {
        Mat4 grip = Translate(g_pose.itemOffset.x, g_pose.itemOffset.y, g_pose.itemOffset.z) *
                    RotZ(g_pose.itemRotation.z) * RotY(g_pose.itemRotation.y) *
                    RotX(g_pose.itemRotation.x);
        Mat4 item = g_world[g_pose.itemBone] * grip;
        glPushMatrix();
        glLoadMatrixf((view * item).m);
        glColor3f(0.62f, 0.63f, 0.66f);
        DrawCmo(g_item, 1, 1, 1);
        glPopMatrix();
    }
    // restore modelview for next frame bookkeeping (not strictly needed)
    (void)view;
}

// Minimal gluPerspective replacement
static void gluPerspectiveHolder(double fovy, double aspect, double zNear, double zFar)
{
    double fh = tan(fovy / 360.0 * 3.14159265358979323846) * zNear;
    double fw = fh * aspect;
    glFrustum(-fw, fw, -fh, fh, zNear, zFar);
}

// ---------------------------------------------------------------- win32
static void CreatePanelControls(HWND parent)
{
    // joints list
    CreateWindowA("LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                  8, 8, 170, 300, parent, (HMENU)IDC_LIST_BONES, nullptr, nullptr);
    for (int b = 0; b < 16; ++b)
        SendDlgItemMessageA(parent, IDC_LIST_BONES, LB_ADDSTRING, 0, (LPARAM)kBoneNames[b]);
    SendDlgItemMessageA(parent, IDC_LIST_BONES, LB_SETCURSEL, g_selectedBone, 0);

    const char *axisNames[3] = {"Pitch X", "Yaw Y", "Roll Z"};
    for (int a = 0; a < 3; ++a)
    {
        char label[32];
        sprintf_s(label, "%s", axisNames[a]);
        CreateWindowA("STATIC", label, WS_CHILD | WS_VISIBLE, 190, 8 + a * 34, 60, 16, parent,
                      nullptr, nullptr, nullptr);
        CreateWindowA(TRACKBAR_CLASSA, nullptr,
                      WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS, 190, 20 + a * 34, 140,
                      24, parent, (HMENU)(IDC_SLIDER_X + a), nullptr, nullptr);
        SendDlgItemMessageA(parent, IDC_SLIDER_X + a, TBM_SETRANGE, TRUE, MAKELPARAM(-180, 180));
        SendDlgItemMessageA(parent, IDC_SLIDER_X + a, TBM_SETPAGESIZE, 0, 5);
        CreateWindowA("STATIC", "0", WS_CHILD | WS_VISIBLE, 336, 14 + a * 34, 36, 16, parent,
                      (HMENU)(IDC_VAL_X + a), nullptr, nullptr);
    }
    CreateWindowA("BUTTON", "Zero joint", WS_CHILD | WS_VISIBLE, 190, 122, 82, 24, parent,
                  (HMENU)IDC_BTN_ZERO, nullptr, nullptr);
    CreateWindowA("BUTTON", "Zero all", WS_CHILD | WS_VISIBLE, 280, 122, 82, 24, parent,
                  (HMENU)IDC_BTN_ZEROALL, nullptr, nullptr);

    // item attach
    CreateWindowA("STATIC", "--- Held item (.cmo) ---", WS_CHILD | WS_VISIBLE, 8, 316, 170, 16,
                  parent, nullptr, nullptr, nullptr);
    CreateWindowA("COMBOBOX", nullptr,
                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 8, 334, 170, 200,
                  parent, (HMENU)IDC_COMBO_ATTACH, nullptr, nullptr);
    for (int b = 0; b < 16; ++b)
        SendDlgItemMessageA(parent, IDC_COMBO_ATTACH, CB_ADDSTRING, 0, (LPARAM)kBoneNames[b]);
    const char *itemLabels[6] = {"Grip X", "Grip Y", "Grip Z", "Rot X", "Rot Y", "Rot Z"};
    for (int a = 0; a < 6; ++a)
    {
        CreateWindowA("STATIC", itemLabels[a], WS_CHILD | WS_VISIBLE, 190 + (a % 3) * 78,
                      334 + (a / 3) * 34, 70, 14, parent, nullptr, nullptr, nullptr);
        CreateWindowA(TRACKBAR_CLASSA, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ, 190 + (a % 3) * 78,
                      346 + (a / 3) * 34, 140, 22, parent, (HMENU)(IDC_ITEM_X + a), nullptr,
                      nullptr);
        SendDlgItemMessageA(parent, IDC_ITEM_X + a, TBM_SETRANGE, TRUE,
                            MAKELPARAM(a < 3 ? -500 : -180, a < 3 ? 500 : 180));
    }
    CreateWindowA("EDIT", g_pose.itemPath, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                  8, 470, 284, 22, parent, (HMENU)IDC_EDIT_ITEM, nullptr, nullptr);
    CreateWindowA("BUTTON", "...", WS_CHILD | WS_VISIBLE, 296, 470, 36, 22, parent,
                  (HMENU)IDC_BTN_BROWSE, nullptr, nullptr);

    // presets
    CreateWindowA("STATIC", "--- Presets ---", WS_CHILD | WS_VISIBLE, 8, 500, 170, 16, parent,
                  nullptr, nullptr, nullptr);
    CreateWindowA("COMBOBOX", nullptr,
                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 8, 518, 240, 200,
                  parent, (HMENU)IDC_COMBO_PRESET, nullptr, nullptr);
    CreateWindowA("BUTTON", "Save", WS_CHILD | WS_VISIBLE, 254, 517, 56, 24, parent,
                  (HMENU)IDC_BTN_SAVE, nullptr, nullptr);
    CreateWindowA("BUTTON", "Load", WS_CHILD | WS_VISIBLE, 314, 517, 56, 24, parent,
                  (HMENU)IDC_BTN_LOAD, nullptr, nullptr);
    CreateWindowA("STATIC", g_status, WS_CHILD | WS_VISIBLE, 8, 548, 360, 40, parent,
                  (HMENU)IDC_STATIC_STATUS, nullptr, nullptr);

    UpdateSlidersFromState();
    RefreshPresets(GetDlgItem(parent, IDC_COMBO_PRESET));
}

static void SyncItemSliders()
{
    int vals[6] = {(int)(g_pose.itemOffset.x * 1000), (int)(g_pose.itemOffset.y * 1000),
                   (int)(g_pose.itemOffset.z * 1000),
                   (int)(g_pose.itemRotation.x * 180.0f / 3.14159265f),
                   (int)(g_pose.itemRotation.y * 180.0f / 3.14159265f),
                   (int)(g_pose.itemRotation.z * 180.0f / 3.14159265f)};
    for (int a = 0; a < 6; ++a)
        SendDlgItemMessageA(g_panel, IDC_ITEM_X + a, TBM_SETPOS, TRUE, vals[a]);
}

static void OnCommand(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);
    switch (id)
    {
        case IDC_LIST_BONES:
            if (code == LBN_SELCHANGE)
            {
                g_selectedBone = (int)SendDlgItemMessageA(g_panel, IDC_LIST_BONES, LB_GETCURSEL, 0, 0);
                UpdateSlidersFromState();
            }
            break;
        case IDC_BTN_ZERO:
            memset(g_pose.euler[g_selectedBone], 0, sizeof(g_pose.euler[g_selectedBone]));
            UpdateSlidersFromState();
            break;
        case IDC_BTN_ZEROALL:
            memset(g_pose.euler, 0, sizeof(g_pose.euler));
            UpdateSlidersFromState();
            break;
        case IDC_COMBO_ATTACH:
            if (code == CBN_SELCHANGE)
                g_pose.itemBone = (int)SendDlgItemMessageA(g_panel, IDC_COMBO_ATTACH, CB_GETCURSEL, 0, 0);
            break;
        case IDC_BTN_BROWSE:
        {
            char file[MAX_PATH] = "";
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "Sub Rosa model\0*.cmo\0All\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameA(&ofn))
            {
                strncpy_s(g_pose.itemPath, file, _TRUNCATE);
                SetDlgItemTextA(g_panel, IDC_EDIT_ITEM, g_pose.itemPath);
                g_hasItem = LoadCmo(g_pose.itemPath, g_item);
            }
            break;
        }
        case IDC_BTN_SAVE:
        {
            char name[64] = "mypose";
            SendDlgItemMessageA(g_panel, IDC_COMBO_PRESET, WM_GETTEXT, (WPARAM)sizeof(name),
                                (LPARAM)name);
            char path[MAX_PATH];
            sprintf_s(path, "%s\\%s.preset", PresetDir().c_str(), name);
            CreateDirectoryA(PresetDir().c_str(), nullptr);
            SavePreset(path);
            RefreshPresets(GetDlgItem(g_panel, IDC_COMBO_PRESET));
            sprintf_s(g_status, "saved %s", path);
            SetDlgItemTextA(g_panel, IDC_STATIC_STATUS, g_status);
            break;
        }
        case IDC_BTN_LOAD:
        {
            int sel = (int)SendDlgItemMessageA(g_panel, IDC_COMBO_PRESET, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_presets.size())
            {
                std::string path = PresetDir() + "\\" + g_presets[sel];
                if (LoadPresetFile(path.c_str()))
                {
                    g_hasItem = LoadCmo(g_pose.itemPath, g_item);
                    SetDlgItemTextA(g_panel, IDC_EDIT_ITEM, g_pose.itemPath);
                    UpdateSlidersFromState();
                    SyncItemSliders();
                    sprintf_s(g_status, "loaded %s", g_presets[sel].c_str());
                }
                else
                    sprintf_s(g_status, "failed to load preset");
                SetDlgItemTextA(g_panel, IDC_STATIC_STATUS, g_status);
            }
            break;
        }
    }
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND)
    {
        OnCommand(g_window, w, l);
        return 0;
    }
    if (m == WM_HSCROLL)
    {
        HWND src = (HWND)l;
        int id = GetDlgCtrlID(src);
        if (id >= IDC_SLIDER_X && id <= IDC_SLIDER_Z)
        {
            g_sliderValues[id - IDC_SLIDER_X] =
                (int)SendDlgItemMessageA(g_panel, id, TBM_GETPOS, 0, 0);
            ApplySliders();
            char buf[32];
            sprintf_s(buf, "%d", g_sliderValues[id - IDC_SLIDER_X]);
            SetDlgItemTextA(g_panel, IDC_VAL_X + (id - IDC_SLIDER_X), buf);
        }
        else if (id >= IDC_ITEM_X && id <= IDC_ITEM_RZ)
        {
            int v = (int)SendDlgItemMessageA(g_panel, id, TBM_GETPOS, 0, 0);
            int k = id - IDC_ITEM_X;
            if (k < 3)
                (&g_pose.itemOffset.x)[k] = v / 1000.0f;
            else
                (&g_pose.itemRotation.x)[k - 3] = v * 3.14159265f / 180.0f;
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static bool dragging = false;
    static POINT last{};
    switch (m)
    {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            g_winW = LOWORD(l);
            g_winH = HIWORD(l);
            MoveWindow(g_glChild, 0, 0, g_winW - kPanelW, g_winH, TRUE);
            MoveWindow(g_panel, g_winW - kPanelW, 0, kPanelW, g_winH, TRUE);
            return 0;
        case WM_COMMAND:
            OnCommand(h, w, l);
            return 0;
        case WM_LBUTTONDOWN:
            dragging = true;
            last = {(short)LOWORD(l), (short)HIWORD(l)};
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP:
            dragging = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE:
            if (dragging)
            {
                POINT cur{(short)LOWORD(l), (short)HIWORD(l)};
                g_camYaw += (cur.x - last.x) * 0.01f;
                g_camPitch += (cur.y - last.y) * 0.01f;
                if (g_camPitch > 1.5f)
                    g_camPitch = 1.5f;
                if (g_camPitch < -1.5f)
                    g_camPitch = -1.5f;
                last = cur;
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            g_camDist *= (GET_WHEEL_DELTA_WPARAM(w) > 0) ? 0.9f : 1.1f;
            if (g_camDist < 0.5f)
                g_camDist = 0.5f;
            if (g_camDist > 12.0f)
                g_camDist = 12.0f;
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show)
{
    InitCommonControls();
    WNDCLASSA wc = {};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(40, 42, 48));
    wc.lpszClassName = "SandiumPoser";
    RegisterClassA(&wc);
    WNDCLASSA pc = wc;
    pc.lpfnWndProc = PanelProc;
    pc.lpszClassName = "SandiumPoserPanel";
    pc.hbrBackground = CreateSolidBrush(RGB(52, 54, 60));
    RegisterClassA(&pc);

    RECT rect = {0, 0, g_winW, g_winH};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowA("SandiumPoser", "Sandium Poser", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                             rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char *slash = strrchr(exePath, '\\');
    if (slash)
        *slash = 0;
    g_exeDir = exePath;

    g_glChild = CreateWindowA("STATIC", nullptr, WS_CHILD | WS_VISIBLE, 0, 0,
                              g_winW - kPanelW, g_winH, g_window, nullptr, instance, nullptr);
    g_panel = CreateWindowA("SandiumPoserPanel", nullptr, WS_CHILD | WS_VISIBLE, g_winW - kPanelW,
                            0, kPanelW, g_winH, g_window, nullptr, instance, nullptr);

    g_dc = GetDC(g_glChild);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    int format = ChoosePixelFormat(g_dc, &pfd);
    SetPixelFormat(g_dc, format, &pfd);
    g_rc = wglCreateContext(g_dc);
    wglMakeCurrent(g_dc, g_rc);

    std::string gameDir = "D:\\SteamLibrary\\steamapps\\common\\Sub Rosa";
    if (__argc > 1)
    {
        std::string arg = __argv[1];
        if (arg.size() > 4 && arg.substr(arg.size() - 4) == ".cmc")
        {
            size_t s = arg.find_last_of("\\/");
            if (s != std::string::npos)
                gameDir = arg.substr(0, s);
        }
    }
    std::string cmc = gameDir + "\\data\\model\\msuit1.cmc";
    g_hasBody = LoadCmc(cmc.c_str(), g_body);
    if (g_hasBody)
    {
        SolveBinds(cmc.c_str());
        sprintf_s(g_status, "model ok: %d verts", (int)g_body.verts.size());
    }
    else
        sprintf_s(g_status, "failed to load %s", cmc.c_str());
    g_hasItem = LoadCmo(g_pose.itemPath, g_item);

    // default working directory = game dir so relative item paths resolve
    SetCurrentDirectoryA(gameDir.c_str());

    CreatePanelControls(g_panel);
    {
        // load a preset passed on the command line, else "9mm" if present
        std::string preset = (__argc > 2) ? __argv[2] : "9mm";
        std::string path = PresetDir() + "\\" + preset + ".preset";
        if (LoadPresetFile(path.c_str()))
        {
            g_hasItem = LoadCmo(g_pose.itemPath, g_item);
            SetDlgItemTextA(g_panel, IDC_EDIT_ITEM, g_pose.itemPath);
            UpdateSlidersFromState();
            SyncItemSliders();
            sprintf_s(g_status, "%s (%s)", g_status, preset.c_str());
            SetDlgItemTextA(g_panel, IDC_STATIC_STATUS, g_status);
        }
    }
    SetDlgItemTextA(g_panel, IDC_STATIC_STATUS, g_status);
    SyncItemSliders();

    ShowWindow(g_window, show);
    MSG msg = {};
    for (;;)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Render();
        SwapBuffers(g_dc);
        Sleep(10);
    }
}
