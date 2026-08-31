// 3D 姿态视图主题常量（颜色/材质集中管理，避免散落硬编码）
// 用法：在视图内声明非视觉子对象 `Theme { id: theme }` 后引用 theme.xxx
import QtQuick

QtObject {
    // 场景
    readonly property color sceneBackground: "#141922"
    readonly property color gridColor: "#1d2634"
    // 舱体
    readonly property color hullColor: "#ff8c2f"
    readonly property color sonarConeColor: "#2fa8ff"
    readonly property color frameColor: "#8b93a3"
    readonly property color thrusterColor: "#3b4657"
    // 艏向指示
    readonly property color headingColor: "#3ddc84"
    // 材质参数
    readonly property real hullMetalness: 0.35
    readonly property real hullRoughness: 0.45
    readonly property real coneRoughness: 0.3
    readonly property real headingEmissive: 0.6
    // 相机与灯光
    readonly property vector3d cameraPosition: Qt.vector3d(0, 150, 420)
    readonly property real cameraPitchDeg: -18
    readonly property real keyLightBrightness: 1.1
    readonly property real fillLightBrightness: 0.45
}
