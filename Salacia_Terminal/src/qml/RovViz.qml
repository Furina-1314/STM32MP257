// 三维舱体姿态视图（Quick3D；OpenGL RHI 由 main.cpp 全局强制）
// 数据绑定：上下文属性 rovViz（RovVizModel，主线程 20Hz 通知）
import QtQuick
import QtQuick3D

View3D {
    id: root

    environment: SceneEnvironment {
        clearColor: "#141922"
        backgroundMode: SceneEnvironment.Color
        antialiasingMode: SceneEnvironment.MSAA
    }

    PerspectiveCamera {
        id: camera
        position: Qt.vector3d(0, 150, 420)
        eulerRotation.x: -18
    }

    DirectionalLight {
        eulerRotation.x: -35
        eulerRotation.y: -40
        brightness: 1.1
    }
    DirectionalLight {
        eulerRotation.x: 25
        eulerRotation.y: 140
        brightness: 0.45
    }

    // 地面参考网格（ROV 本体坐标系）
    Model {
        source: "#Rectangle"
        scale: Qt.vector3d(4.0, 4.0, 1.0)
        eulerRotation.x: -90
        materials: PrincipledMaterial {
            baseColor: "#1d2634"
        }
    }

    // 舱体节点：姿态四元数直接驱动（w, x, y, z）
    Node {
        id: rovBody
        rotation: Qt.quaternion(rovViz.qw, rovViz.qx, rovViz.qy, rovViz.qz)

        // 主耐压舱（长方体，艏向 +X）
        Model {
            source: "#Cube"
            scale: Qt.vector3d(2.2, 0.55, 0.85)
            materials: PrincipledMaterial {
                baseColor: "#ff8c2f"
                metalness: 0.35
                roughness: 0.45
            }
        }

        // 艏部声呐/摄像头锥
        Model {
            source: "#Cone"
            position: Qt.vector3d(135, 0, 0)
            eulerRotation.z: -90
            scale: Qt.vector3d(0.35, 0.5, 0.35)
            materials: PrincipledMaterial {
                baseColor: "#2fa8ff"
                roughness: 0.3
            }
        }

        // 顶部框架
        Model {
            source: "#Cube"
            position: Qt.vector3d(0, 48, 0)
            scale: Qt.vector3d(1.6, 0.12, 0.6)
            materials: PrincipledMaterial { baseColor: "#8b93a3" }
        }

        // 四个推进器（水平）
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(95, 0, 62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: "#3b4657" }
        }
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(95, 0, -62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: "#3b4657" }
        }
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(-95, 0, 62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: "#3b4657" }
        }
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(-95, 0, -62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: "#3b4657" }
        }
    }

    // 艏向指示（世界坐标固定，不随舱体旋转）
    Model {
        source: "#Cone"
        position: Qt.vector3d(260, 95, 0)
        eulerRotation.z: -90
        scale: Qt.vector3d(0.3, 0.6, 0.3)
        materials: PrincipledMaterial {
            baseColor: "#3ddc84"
            emissiveness: 0.6
        }
    }
}
