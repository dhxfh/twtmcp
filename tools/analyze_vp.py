import math

# 从链路读到的矩阵 (行主序显示, 4x4)
M = [
    [ 0.997888,  -0.000105,   0.000000,   0.004202],
    [ 0.004193,   0.024926,   0.000000,  -0.999927],
    [ 0.000000,   2.195232,   0.000000,   0.011354],
    [-29953.203125, -1084.974243, 1.000000, 29832.886719],
]

print("=== 原样 (行主序) ===")
for r in M: print('  '+' '.join(f'{v:13.6f}' for v in r))

# 判据1: 左上3x3行模长(是否旋转矩阵)
print("\n=== 左上 3x3 行模长 ===")
for i in range(3):
    n = math.sqrt(M[i][0]**2+M[i][1]**2+M[i][2]**2)
    print(f'  row{i}: {n:.6f}')
print("=== 左上 3x3 列模长 ===")
for j in range(3):
    n = math.sqrt(M[0][j]**2+M[1][j]**2+M[2][j]**2)
    print(f'  col{j}: {n:.6f}')

# 判据2: 参考源码 WorldToScreen 的列主序约定
#   w   = m[3]*x + m[7]*y + m[11]*z + m[15]
#   cx  = m[0]*x + m[4]*y + m[8]*z + m[12]
#   cy  = m[1]*x + m[5]*y + m[9]*z + m[13]
# 若这是 ViewProjection, 则 齐次分量 w 应为 深度
print("\n=== 按源码列主序约定解释 (索引=行主序展平) ===")
f = [M[r][c] for r in range(4) for c in range(4)]
print(f'  m[3],m[7],m[11],m[15] (w 行)   = {f[3]:.6f}, {f[7]:.6f}, {f[11]:.6f}, {f[15]:.6f}')
print(f'  m[0],m[4],m[8],m[12]  (clipX)  = {f[0]:.6f}, {f[4]:.6f}, {f[8]:.6f}, {f[12]:.6f}')
print(f'  m[1],m[5],m[9],m[13]  (clipY)  = {f[1]:.6f}, {f[5]:.6f}, {f[9]:.6f}, {f[13]:.6f}')
print(f'  m[2],m[6],m[10],m[14] (clipZ)  = {f[2]:.6f}, {f[6]:.6f}, {f[10]:.6f}, {f[14]:.6f}')

# 试试: 相机世界位置从第4行反推 (若是视图矩阵, 平移 = -R^T * t)
# 视图矩阵: V = [R | t], 相机世界位置 = -R^T * t
tx, ty, tz = M[3][0], M[3][1], M[3][2]
print(f"\n=== 若为视图矩阵, 平移分量 t = ({tx:.3f}, {ty:.3f}, {tz:.3f}) ===")
# 用转置(即旋转的逆)算相机世界坐标
R = [[M[i][j] for j in range(3)] for i in range(3)]
Rt = [[R[j][i] for j in range(3)] for i in range(3)]
cx = -(Rt[0][0]*tx + Rt[0][1]*ty + Rt[0][2]*tz)
cy = -(Rt[1][0]*tx + Rt[1][1]*ty + Rt[1][2]*tz)
cz = -(Rt[2][0]*tx + Rt[2][1]*ty + Rt[2][2]*tz)
print(f'  相机世界位置 (推算) ≈ ({cx:.2f}, {cy:.2f}, {cz:.2f})')
