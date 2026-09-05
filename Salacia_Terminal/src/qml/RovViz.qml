// 三维舱体姿态视图（Quick3D；OpenGL RHI 由 main.cpp 全局强制）
// 数据绑定：上下文属性 rovViz（RovVizModel，主线程 20Hz 通知）
// 视觉常量集中于 Theme.qml（Phase 1 迁移，禁散落硬编码）
import QtQuick
import QtQuick3D

View3D {
    id: root

    Theme { id: theme }

    environment: SceneEnvironment {
        clearColor: theme.sceneBackground
        backgroundMode: SceneEnvironment.Color
        antialiasingMode: SceneEnvironment.MSAA
    }

    PerspectiveCamera {
        id: camera
        position: theme.cameraPosition
        eulerRotation.x: theme.cameraPitchDeg
    }

    DirectionalLight {
        eulerRotation.x: -35
        eulerRotation.y: -40
        brightness: theme.keyLightBrightness
    }
    DirectionalLight {
        eulerRotation.x: 25
        eulerRotation.y: 140
        brightness: theme.fillLightBrightness
    }

    // 地面参考网格（ROV 本体坐标系）
    Model {
        source: "#Rectangle"
        scale: Qt.vector3d(4.0, 4.0, 1.0)
        eulerRotation.x: -90
        materials: PrincipledMaterial {
            baseColor: theme.gridColor
        }
    }

    // 舱体节点：姿态四元数直接驱动（w, x, y, z）
    // rovViz 判空：关闭析构期模型先于视图销毁，绑定再求值时不告 TypeError
    Node {
        id: rovBody
        rotation: rovViz
               ? Qt.quaternion(rovViz.qw, rovViz.qx, rovViz.qy, rovViz.qz)
               : Qt.quaternion(1, 0, 0, 0)

        // 主耐压舱（长方体，艏向 +X）
        Model {
            source: "#Cube"
            scale: Qt.vector3d(2.2, 0.55, 0.85)
            materials: PrincipledMaterial {
                baseColor: theme.hullColor
                metalness: theme.hullMetalness
                roughness: theme.hullRoughness
            }
        }

        // 艏部声呐/摄像头锥
        Model {
            source: "#Cone"
            position: Qt.vector3d(135, 0, 0)
            eulerRotation.z: -90
            scale: Qt.vector3d(0.35, 0.5, 0.35)
            materials: PrincipledMaterial {
                baseColor: theme.sonarConeColor
                roughness: theme.coneRoughness
            }
        }

        // 顶部框架
        Model {
            source: "#Cube"
            position: Qt.vector3d(0, 48, 0)
            scale: Qt.vector3d(1.6, 0.12, 0.6)
            materials: PrincipledMaterial { baseColor: theme.frameColor }
        }

        // 四个推进器（水平）
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(95, 0, 62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: theme.thrusterColor }
        }
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(95, 0, -62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: theme.thrusterColor }
        }
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(-95, 0, 62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: theme.thrusterColor }
        }
        Model {
            source: "#Cylinder"
            position: Qt.vector3d(-95, 0, -62)
            eulerRotation.x: 90
            scale: Qt.vector3d(0.22, 0.28, 0.22)
            materials: PrincipledMaterial { baseColor: theme.thrusterColor }
        }
    }

    // 艏向指示（世界坐标固定，不随舱体旋转）
    Model {
        source: "#Cone"
        position: Qt.vector3d(260, 95, 0)
        eulerRotation.z: -90
        scale: Qt.vector3d(0.3, 0.6, 0.3)
        materials: PrincipledMaterial {
            baseColor: theme.headingColor
            // Qt 6.11 PrincipledMaterial 无 emissiveness；发光强度走 emissiveFactor
            emissiveFactor: Qt.vector3d(theme.headingEmissive, theme.headingEmissive,
                                        theme.headingEmissive)
        }
    }
}
