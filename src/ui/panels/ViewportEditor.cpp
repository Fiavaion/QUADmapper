#include "ViewportEditor.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGeometry.hpp"
#include "scene/Scene.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "project/Project.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <glm/glm.hpp>

#include <memory>
#include <unordered_map>

namespace quadmapper {

void ViewportEditor::render(EditorContext& ctx) {
    // Same-named aliases keep the moved render()/interaction body verbatim
    // (LESSON-ARCH-006). All editing state lives in EditorState; the manager
    // pointers come from the per-frame EditorContext.
    EditorState& es = ctx.es;
    auto* m_renderer = ctx.renderer;
    auto* m_window = ctx.window;
    auto* m_project = ctx.project;

    auto& quadManager = es.quadManager();
    auto& quadTextures = es.quadTextures;

    auto& cornerHoveredVertex = es.cornerHoveredVertex;
    auto& cornerDraggedVertex = es.cornerDraggedVertex;
    auto& cornerSelectedVertex = es.cornerSelectedVertex;
    auto& cornerDragStartMousePos = es.cornerDragStartMousePos;
    auto& cornerDragStartPos = es.cornerDragStartPos;

    auto& meshHoveredVertex = es.meshHoveredVertex;
    auto& meshDraggedVertex = es.meshDraggedVertex;
    auto& meshSelectedVertex = es.meshSelectedVertex;
    auto& meshDragStartMousePos = es.meshDragStartMousePos;

    auto& snapActive = es.snapActive;
    auto& snapTargetPos = es.snapTargetPos;
    auto& meshDragStartOffset = es.meshDragStartOffset;

    auto& videoTextureId = es.videoTextureId;
    auto& videoTextureWidth = es.videoTextureWidth;
    auto& videoTextureHeight = es.videoTextureHeight;
    auto& videoAssignedQuadId = es.videoAssignedQuadId;

    // Disable depth test for 2D rendering
    glDisable(GL_DEPTH_TEST);

    // Render all quads
    if (m_renderer) {
        m_renderer->beginFrame();

        // Build texture pointer map for rendering
        std::unordered_map<UUID, Texture*> texturePtrs;
        for (const auto& [id, texPtr] : quadTextures) {
            texturePtrs[id] = texPtr.get();
        }

        // Handle video texture rendering for assigned quad
        auto& videoTexWrapper = es.videoTexWrapper;  // Owned by EditorState (see ADR-031 TD-02)
        if (!videoAssignedQuadId.isNull() && videoTextureId != 0) {
            // Use raw OpenGL texture binding for video
            videoTexWrapper.setGLTextureId(videoTextureId, videoTextureWidth, videoTextureHeight);
            texturePtrs[videoAssignedQuadId] = &videoTexWrapper;
        }

        m_renderer->renderQuads(quadManager, texturePtrs);

        // Render handles for selected quad based on mode
        Quad* selectedQuadForHandles = quadManager.getPrimarySelectedQuad();
        if (selectedQuadForHandles) {
            MeshMode mode = selectedQuadForHandles->getGeometry().getMeshMode();
            if (mode == MeshMode::MESH_EDIT) {
                // Draw the control-grid wireframe so division lines are visible,
                // then the vertex handles on top.
                m_renderer->renderMeshGrid(*selectedQuadForHandles,
                                           glm::vec4(0.0f, 0.8f, 1.0f, 0.6f), 1.0f);
                m_renderer->renderMeshHandles(*selectedQuadForHandles, meshHoveredVertex, meshDraggedVertex, meshSelectedVertex);
            } else {
                // CORNERS_ONLY mode (default) - show just the 4 corner handles
                m_renderer->renderCornerHandles(*selectedQuadForHandles, cornerHoveredVertex, cornerDraggedVertex, cornerSelectedVertex);
            }

            // Render snap indicator if actively snapping to another quad's vertex
            if (snapActive) {
                m_renderer->renderSnapIndicator(snapTargetPos);
            }
        }

        m_renderer->endFrame();
    }

    // Handle vertex interaction (before ImGui to capture mouse before UI)
    {
        Quad* selectedQuad = quadManager.getPrimarySelectedQuad();
        if (selectedQuad) {
            QuadGeometry& geometry = selectedQuad->getGeometry();
            MeshMode mode = geometry.getMeshMode();

            // Get mouse position in normalized coordinates.
            // glfwGetCursorPos returns screen/window (logical) coordinates, so it
            // must be normalized by the WINDOW size — not the framebuffer size.
            // On a display with scaling != 100% the framebuffer (pixels) is larger
            // than the window (logical), and dividing by the framebuffer skews every
            // hit-test by the DPI factor, making the on-canvas handles unclickable.
            double mouseX, mouseY;
            glfwGetCursorPos(m_window, &mouseX, &mouseY);

            int winWidth, winHeight;
            glfwGetWindowSize(m_window, &winWidth, &winHeight);

            Vec2 mouseNorm(
                static_cast<float>(mouseX) / winWidth,
                static_cast<float>(mouseY) / winHeight
            );

            bool mouseDown = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

            constexpr float HIT_RADIUS = 0.015f;  // Slightly larger hit radius for corners
            constexpr float SNAP_RADIUS = 0.02f;  // Snap threshold for vertex snapping

            // Reset snap state at start of each frame
            snapActive = false;

            // Helper lambda to find snap target from other quads
            auto findSnapTarget = [&](const Vec2& pos, const UUID& excludeQuadId) -> Vec2 {
                Vec2 snappedPos = pos;
                float closestSnapDist = SNAP_RADIUS;

                // Check all other quads for snap targets
                const auto& allQuads = quadManager.getQuads();
                for (const auto& otherQuad : allQuads) {
                    if (otherQuad->getId() == excludeQuadId) continue;  // Skip same quad

                    const auto& otherCorners = otherQuad->getGeometry().getCorners();
                    for (int i = 0; i < 4; ++i) {
                        float dist = glm::length(pos - otherCorners[i]);
                        if (dist < closestSnapDist) {
                            closestSnapDist = dist;
                            snappedPos = otherCorners[i];
                        }
                    }

                    // Also check mesh vertices if in mesh edit mode
                    if (otherQuad->getGeometry().getMeshMode() == MeshMode::MESH_EDIT) {
                        auto meshPositions = otherQuad->getGeometry().generateMeshPositions();
                        for (const auto& meshPos : meshPositions) {
                            float dist = glm::length(pos - meshPos);
                            if (dist < closestSnapDist) {
                                closestSnapDist = dist;
                                snappedPos = meshPos;
                            }
                        }
                    }
                }

                // Set snap indicator state if snapping occurred
                if (snappedPos != pos) {
                    snapActive = true;
                    snapTargetPos = snappedPos;
                }

                return snappedPos;
            };

            // Grid-division insert tool (ADR-038): when armed, pressing inside the
            // quad inserts ONE division at the clicked param position and then lets
            // you DRAG it to fine-position the line; releasing commits and disarms
            // (one-shot, so dragging vertices resumes). Esc cancels. This avoids the
            // trap of an armed mode silently swallowing every click with no way out.
            if (es.gridInsert != EditorState::GridInsert::None) {
                const bool isColumn = (es.gridInsert == EditorState::GridInsert::Column);
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    es.gridInsert = EditorState::GridInsert::None;
                    es.gridInsertMouseLatch = false;
                    es.gridInsertDragIndex = -1;
                } else if (mouseDown) {
                    Vec2 uv = QuadGeometry::inverseBilinear(geometry.getCorners(), mouseNorm);
                    if (!es.gridInsertMouseLatch) {
                        // Press edge: insert the new line where the user clicked.
                        es.gridInsertMouseLatch = true;
                        if (uv.x > 0.001f && uv.x < 0.999f &&
                            uv.y > 0.001f && uv.y < 0.999f) {
                            es.gridInsertDragIndex = isColumn
                                ? geometry.insertUDivision(uv.x)
                                : geometry.insertVDivision(uv.y);
                            m_project->setModified(true);
                        }
                    } else if (es.gridInsertDragIndex >= 0) {
                        // Held: drag the just-inserted line to fine-position it.
                        if (isColumn) geometry.moveUDivision(es.gridInsertDragIndex, uv.x);
                        else          geometry.moveVDivision(es.gridInsertDragIndex, uv.y);
                        m_project->setModified(true);
                    }
                } else {
                    // Release: commit + disarm only if a line was actually placed.
                    es.gridInsertMouseLatch = false;
                    if (es.gridInsertDragIndex >= 0) {
                        es.gridInsert = EditorState::GridInsert::None;
                        es.gridInsertDragIndex = -1;
                    }
                }
            }
            const bool insertArmed = (es.gridInsert != EditorState::GridInsert::None);

            if (mode == MeshMode::CORNERS_ONLY) {
                // ===== CORNER PIN MODE (Default) =====
                const auto& corners = geometry.getCorners();

                if (cornerDraggedVertex >= 0) {
                    // Currently dragging a corner
                    if (mouseDown) {
                        Vec2 mouseDelta = mouseNorm - cornerDragStartMousePos;
                        Vec2 newPos = cornerDragStartPos + mouseDelta;

                        // Apply snapping to other quads' vertices
                        newPos = findSnapTarget(newPos, selectedQuad->getId());

                        geometry.setCorner(cornerDraggedVertex, newPos);
                        m_project->setModified(true);
                    } else {
                        // Mouse released
                        cornerSelectedVertex = cornerDraggedVertex;
                        cornerDraggedVertex = -1;
                    }
                } else {
                    // Check for hover over corners
                    cornerHoveredVertex = -1;
                    float closestDist = HIT_RADIUS;

                    for (int i = 0; i < 4; ++i) {
                        float dist = glm::length(mouseNorm - corners[i]);
                        if (dist < closestDist) {
                            closestDist = dist;
                            cornerHoveredVertex = i;
                        }
                    }

                    // Start drag if mouse pressed on a corner (not while the
                    // division-insert tool is armed).
                    if (mouseDown && cornerHoveredVertex >= 0 && !insertArmed) {
                        cornerDraggedVertex = cornerHoveredVertex;
                        cornerSelectedVertex = cornerHoveredVertex;
                        cornerDragStartMousePos = mouseNorm;
                        cornerDragStartPos = corners[cornerHoveredVertex];
                    }
                }

                // Keyboard controls for corners
                ImGuiIO& cornerIO = ImGui::GetIO();
                if (!cornerIO.WantTextInput && !cornerIO.WantCaptureKeyboard) {
                    // Tab: Cycle through corners (TL, TR, BR, BL)
                    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                        if (cornerIO.KeyShift) {
                            cornerSelectedVertex = (cornerSelectedVertex <= 0) ? 3 : cornerSelectedVertex - 1;
                        } else {
                            cornerSelectedVertex = (cornerSelectedVertex + 1) % 4;
                        }
                    }

                    // Arrow keys: Move selected corner
                    if (cornerSelectedVertex >= 0 && cornerSelectedVertex < 4) {
                        float moveAmount = 0.001f;
                        if (cornerIO.KeyShift) moveAmount *= 10.0f;
                        if (cornerIO.KeyAlt) moveAmount *= 0.1f;

                        Vec2 pos = corners[cornerSelectedVertex];
                        bool moved = false;

                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                            pos.y -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                            pos.y += moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                            pos.x -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                            pos.x += moveAmount;
                            moved = true;
                        }

                        if (moved) {
                            geometry.setCorner(cornerSelectedVertex, pos);
                            m_project->setModified(true);
                        }
                    }
                }

                // Reset mesh state when in corner mode
                meshHoveredVertex = -1;
                meshDraggedVertex = -1;
                meshSelectedVertex = -1;

            } else {
                // ===== MESH EDIT MODE =====
                auto positions = geometry.generateMeshPositions();
                auto bilinearPositions = geometry.generateBilinearPositions();
                int vertexCount = geometry.getVertexCount();

                if (meshDraggedVertex >= 0) {
                    if (mouseDown) {
                        Vec2 mouseDelta = mouseNorm - meshDragStartMousePos;
                        Vec2 newOffset = meshDragStartOffset + mouseDelta;

                        // Calculate actual position, apply snapping, then compute offset
                        Vec2 bilinearPos = bilinearPositions[meshDraggedVertex];
                        Vec2 actualPos = bilinearPos + newOffset;
                        Vec2 snappedPos = findSnapTarget(actualPos, selectedQuad->getId());
                        newOffset = snappedPos - bilinearPos;

                        geometry.setMeshVertexByIndex(meshDraggedVertex, newOffset);
                        m_project->setModified(true);
                    } else {
                        meshSelectedVertex = meshDraggedVertex;
                        meshDraggedVertex = -1;
                    }
                } else {
                    meshHoveredVertex = -1;
                    float closestDist = 0.012f;  // Mesh handles are smaller

                    for (size_t i = 0; i < positions.size(); ++i) {
                        float dist = glm::length(mouseNorm - positions[i]);
                        if (dist < closestDist) {
                            closestDist = dist;
                            meshHoveredVertex = static_cast<int>(i);
                        }
                    }

                    if (mouseDown && meshHoveredVertex >= 0 && !insertArmed) {
                        meshDraggedVertex = meshHoveredVertex;
                        meshSelectedVertex = meshHoveredVertex;
                        meshDragStartMousePos = mouseNorm;
                        meshDragStartOffset = geometry.getMeshVertexByIndex(meshHoveredVertex);
                    }
                }

                // Keyboard controls for mesh editing
                ImGuiIO& meshIO = ImGui::GetIO();
                if (!meshIO.WantTextInput && !meshIO.WantCaptureKeyboard) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                        if (meshIO.KeyShift) {
                            meshSelectedVertex = (meshSelectedVertex <= 0) ? vertexCount - 1 : meshSelectedVertex - 1;
                        } else {
                            meshSelectedVertex = (meshSelectedVertex + 1) % vertexCount;
                        }
                    }

                    if (meshSelectedVertex >= 0 && meshSelectedVertex < vertexCount) {
                        float moveAmount = 0.001f;
                        if (meshIO.KeyShift) moveAmount *= 10.0f;
                        if (meshIO.KeyAlt) moveAmount *= 0.1f;

                        Vec2 offset = geometry.getMeshVertexByIndex(meshSelectedVertex);
                        bool moved = false;

                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                            offset.y -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                            offset.y += moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                            offset.x -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                            offset.x += moveAmount;
                            moved = true;
                        }

                        if (moved) {
                            geometry.setMeshVertexByIndex(meshSelectedVertex, offset);
                            m_project->setModified(true);
                        }
                    }
                }

                // Reset corner state when in mesh mode
                cornerHoveredVertex = -1;
                cornerDraggedVertex = -1;
                cornerSelectedVertex = -1;
            }
        } else {
            // No quad selected, reset all state
            cornerHoveredVertex = -1;
            cornerDraggedVertex = -1;
            cornerSelectedVertex = -1;
            meshHoveredVertex = -1;
            meshDraggedVertex = -1;
            meshSelectedVertex = -1;
        }
    }

    // Re-enable depth test
    glEnable(GL_DEPTH_TEST);
}

}  // namespace quadmapper
