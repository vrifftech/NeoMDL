#include "wx/ModelCanvas.hpp"
#include "core/RoomSurfaceMesh.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#if defined(__WXMAC__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace neomdl::ui {
wxDEFINE_EVENT(EVT_WALKMESH_EDIT,WalkmeshEvent);
wxDEFINE_EVENT(EVT_ROOM_EDGE_PICKED,wxCommandEvent);

model::Vec3 walkmeshMaterialColor(std::uint32_t material) {
    static const auto colors=[] {
        std::array<model::Vec3,32> values;
        for(std::size_t i=0;i<values.size();++i) {
            const auto angle=float(i)*2.39996323f;
            values[i]={0.6f+0.35f*std::cos(angle),0.6f+0.35f*std::cos(angle+2.0943951f),0.6f+0.35f*std::cos(angle+4.1887902f)};
        }
        return values;
    }();
    return colors[material&31u];
}

void ModelCanvas::setWalkmeshLayers(std::vector<WalkmeshLayer> layers) {
    cancelWalkmeshStroke();walkmeshLayers_=std::move(layers);walkmeshSelection_.clear();Refresh(false);
}

void ModelCanvas::configureWalkmesh(bool visible,bool xray,WalkmeshTool tool,std::string room) {
    if(tool!=walkmeshTool_||room!=walkmeshRoom_||visible!=walkmeshVisible_)cancelWalkmeshStroke();
    walkmeshVisible_=visible;walkmeshXray_=xray;walkmeshTool_=tool;walkmeshRoom_=std::move(room);Refresh(false);
}

void ModelCanvas::setWalkmeshSelection(std::set<WalkmeshFace> faces) {
    walkmeshSelection_=std::move(faces);Refresh(false);
}

void ModelCanvas::emitWalkmesh(AreaGizmoPhase phase,bool painting,std::optional<WalkmeshFace> face) {
    WalkmeshEvent event(EVT_WALKMESH_EDIT,GetId());event.SetEventObject(this);
    event.phase=phase;event.painting=painting;event.face=std::move(face);ProcessWindowEvent(event);
}

void ModelCanvas::cancelWalkmeshStroke() {
    if(!walkmeshDrag_)return;
    const auto painting=walkmeshDrag_->painting;walkmeshDrag_.reset();
    walkmeshSelection_=std::move(walkmeshSelectionBefore_);
    if(HasCapture())ReleaseMouse();
    emitWalkmesh(AreaGizmoPhase::Cancel,painting);Refresh(false);
}

std::optional<WalkmeshFace> ModelCanvas::pickWalkmesh(wxPoint point) const {
    const auto& camera=pickingCamera_.value();const auto scale=GetContentScaleFactor();
    const auto ray=engineCameraHitSegment(camera,int(std::lround(point.x*scale)),
        camera.viewport.height-int(std::lround(point.y*scale)),document_->model().game);
    if(!ray)return {};
    auto closest=std::numeric_limits<float>::max();std::optional<WalkmeshFace> result;
    for(const auto& layer:walkmeshLayers_) {
        if(!walkmeshRoom_.empty()&&layer.room!=walkmeshRoom_)continue;
        RoomSurfaceHitInfo hit;
        if(!engineRoomHitAll(*layer.mesh,ray->start,ray->end,hit))continue;
        const auto dx=hit.position.x-ray->start.x,dy=hit.position.y-ray->start.y,dz=hit.position.z-ray->start.z;
        const auto distance=dx*dx+dy*dy+dz*dz;
        if(distance<closest) {closest=distance;result=WalkmeshFace{layer.room,std::uint32_t(hit.face)};}
    }
    return result;
}

bool ModelCanvas::handleWalkmesh(wxMouseEvent& event) {
    if(walkmeshDrag_) {
        if(event.RightDown()) {cancelWalkmeshStroke();return true;}
        if(event.Dragging()||event.LeftUp()) {
            walkmeshDrag_->current=event.GetPosition();
            if(walkmeshDrag_->painting) {
                if(const auto face=pickWalkmesh(event.GetPosition())) {
                    walkmeshSelection_.insert(*face);emitWalkmesh(AreaGizmoPhase::Preview,true,face);
                }
            }
            if(event.LeftUp()) {
                const auto drag=*walkmeshDrag_;walkmeshDrag_.reset();if(HasCapture())ReleaseMouse();
                if(drag.painting)emitWalkmesh(AreaGizmoPhase::Commit,true);
                else {
                    if(!drag.additive)walkmeshSelection_.clear();
                    if(std::abs(drag.current.x-drag.start.x)>FromDIP(4)||std::abs(drag.current.y-drag.start.y)>FromDIP(4)) {
                        const wxRect box(wxPoint(std::min(drag.start.x,drag.current.x),std::min(drag.start.y,drag.current.y)),
                            wxPoint(std::max(drag.start.x,drag.current.x),std::max(drag.start.y,drag.current.y)));
                        for(const auto& layer:walkmeshLayers_) {
                            if(!walkmeshRoom_.empty()&&layer.room!=walkmeshRoom_)continue;
                            for(std::size_t i=0;i<layer.mesh->faces.size();++i) {
                                const auto& face=layer.mesh->faces[i];model::Vec3 center{};
                                for(const auto index:face.vertices) {const auto& v=layer.mesh->vertices[index];center.x+=v[0]/3;center.y+=v[1]/3;center.z+=v[2]/3;}
                                const auto screen=gizmoScreenPoint(center);
                                if(screen.z>pickingCamera_->nearClip&&screen.z<sceneFarClip_&&box.Contains(int(screen.x),int(screen.y)))
                                    walkmeshSelection_.emplace(layer.room,std::uint32_t(i));
                            }
                        }
                    } else if(const auto face=pickWalkmesh(drag.current)) {
                        if(drag.additive&&walkmeshSelection_.count(*face))walkmeshSelection_.erase(*face);
                        else walkmeshSelection_.insert(*face);
                    }
                    emitWalkmesh(AreaGizmoPhase::Commit,false);
                }
            }
            Refresh(false);
        }
        return true;
    }
    if(!walkmeshVisible_||walkmeshTool_==WalkmeshTool::View||event.AltDown()||event.ControlDown()||
        !event.LeftDown()||event.RightIsDown()||event.MiddleIsDown())return false;
    resetCameraInput();selectionStart_.reset();meshPick_.reset();SetFocus();
    walkmeshSelectionBefore_=walkmeshSelection_;
    walkmeshDrag_=WalkmeshDrag{event.GetPosition(),event.GetPosition(),event.ShiftDown(),walkmeshTool_==WalkmeshTool::Paint};
    CaptureMouse();
    if(walkmeshDrag_->painting) {
        walkmeshSelection_.clear();emitWalkmesh(AreaGizmoPhase::Begin,true);
        if(const auto face=pickWalkmesh(event.GetPosition())) {walkmeshSelection_.insert(*face);emitWalkmesh(AreaGizmoPhase::Preview,true,face);}
    }
    return true;
}

void ModelCanvas::drawWalkmeshOverlay() {
    if(!walkmeshVisible_)return;
    glPushAttrib(GL_ENABLE_BIT|GL_LINE_BIT|GL_CURRENT_BIT|GL_DEPTH_BUFFER_BIT|GL_POLYGON_BIT|GL_COLOR_BUFFER_BIT);
    glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glDisable(GL_CULL_FACE);glDisable(GL_FOG);
    if(walkmeshXray_)glDisable(GL_DEPTH_TEST);else glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(-1,-1);
    glPushMatrix();glLoadMatrixf(cameraView_.values.data());
    glBegin(GL_TRIANGLES);
    for(const auto& layer:walkmeshLayers_) {
        if(!walkmeshRoom_.empty()&&layer.room!=walkmeshRoom_)continue;
        auto selected=walkmeshSelection_.lower_bound({layer.room,0});
        for(std::size_t i=0;i<layer.mesh->faces.size();++i) {
            const auto& face=layer.mesh->faces[i];
            const auto color=walkmeshMaterialColor(face.material);
            if(selected!=walkmeshSelection_.end()&&selected->first==layer.room&&selected->second==i) {glColor4f(1,0.9f,0.15f,0.85f);++selected;}
            else glColor4f(color.x,color.y,color.z,0.6f);
            for(const auto vertex:face.vertices)glVertex3fv(layer.mesh->vertices[vertex].data());
        }
    }
    glEnd();glDisable(GL_POLYGON_OFFSET_FILL);glLineWidth(float(GetContentScaleFactor()));
    glColor4f(0.05f,0.06f,0.08f,0.8f);glBegin(GL_LINES);
    for(const auto& layer:walkmeshLayers_) {
        if(!walkmeshRoom_.empty()&&layer.room!=walkmeshRoom_)continue;
        for(const auto& face:layer.mesh->faces)for(std::size_t edge=0;edge<3;++edge) {
            glVertex3fv(layer.mesh->vertices[face.vertices[edge]].data());glVertex3fv(layer.mesh->vertices[face.vertices[(edge+1)%3]].data());
        }
    }
    glEnd();
    if(walkmeshDrag_&&!walkmeshDrag_->painting) {
        const auto size=GetClientSize();glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glOrtho(0,size.x,size.y,0,-1,1);
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();glColor4f(1,0.9f,0.15f,1);glBegin(GL_LINE_LOOP);
        const auto& drag=*walkmeshDrag_;glVertex2i(drag.start.x,drag.start.y);glVertex2i(drag.current.x,drag.start.y);
        glVertex2i(drag.current.x,drag.current.y);glVertex2i(drag.start.x,drag.current.y);glEnd();
        glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(GL_MODELVIEW);
    }
    glPopMatrix();glPopAttrib();
}

void ModelCanvas::setRoomConnections(std::vector<RoomConnection> connections) {
    roomConnections_=std::move(connections);selectedRoomConnection_.reset();Refresh(false);
}

void ModelCanvas::configureRoomConnections(bool visible,bool xray,std::optional<std::size_t> selected) {
    if(visible!=roomConnectionsVisible_) {meshPick_.reset();selectionStart_.reset();}
    roomConnectionsVisible_=visible;roomConnectionsXray_=xray;selectedRoomConnection_=selected;Refresh(false);
}

std::optional<std::size_t> ModelCanvas::pickRoomConnection(wxPoint point) const {
    auto closest=float(FromDIP(8));std::optional<std::size_t> result;
    for(std::size_t i=0;i<roomConnections_.size();++i) {
        const auto& edge=roomConnections_[i];const auto a=gizmoScreenPoint(edge.first),b=gizmoScreenPoint(edge.second);
        if(a.z<=pickingCamera_->nearClip||b.z<=pickingCamera_->nearClip)continue;
        const auto dx=b.x-a.x,dy=b.y-a.y,squared=dx*dx+dy*dy;
        const auto t=squared==0?0.0f:std::clamp(((point.x-a.x)*dx+(point.y-a.y)*dy)/squared,0.0f,1.0f);
        const auto distance=std::hypot(point.x-a.x-dx*t,point.y-a.y-dy*t);
        if(distance<closest) {closest=distance;result=i;}
    }
    return result;
}

void ModelCanvas::drawRoomConnections() {
    if(!roomConnectionsVisible_)return;
    glPushAttrib(GL_ENABLE_BIT|GL_LINE_BIT|GL_CURRENT_BIT|GL_DEPTH_BUFFER_BIT);
    glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glDisable(GL_FOG);glDisable(GL_BLEND);
    if(roomConnectionsXray_)glDisable(GL_DEPTH_TEST);else glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);glPushMatrix();glLoadMatrixf(cameraView_.values.data());
    glLineWidth(2.0f*float(GetContentScaleFactor()));glBegin(GL_LINES);
    for(const auto& edge:roomConnections_) {
        if(edge.target==-1)glColor3f(0.6f,0.65f,0.7f);
        else if(!edge.targetEdge)glColor3f(1,0.25f,0.2f);
        else if(!edge.aligned||!edge.reciprocal)glColor3f(1,0.6f,0.15f);
        else glColor3f(0.2f,1,0.4f);
        glVertex3f(edge.first.x,edge.first.y,edge.first.z);glVertex3f(edge.second.x,edge.second.y,edge.second.z);
    }
    glEnd();
    if(selectedRoomConnection_) {
        const auto& edge=roomConnections_.at(*selectedRoomConnection_);glLineWidth(5.0f*float(GetContentScaleFactor()));
        glColor3f(1,0.9f,0.15f);glBegin(GL_LINES);glVertex3f(edge.first.x,edge.first.y,edge.first.z);glVertex3f(edge.second.x,edge.second.y,edge.second.z);glEnd();
        if(edge.targetEdge) {
            glColor3f(0.2f,0.8f,1);glBegin(GL_LINES);glVertex3f(edge.targetFirst.x,edge.targetFirst.y,edge.targetFirst.z);glVertex3f(edge.targetSecond.x,edge.targetSecond.y,edge.targetSecond.z);glEnd();
            glLineWidth(2.0f*float(GetContentScaleFactor()));glColor3f(1,0.6f,0.15f);glBegin(GL_LINES);
            glVertex3f(edge.first.x,edge.first.y,edge.first.z);glVertex3f(edge.targetSecond.x,edge.targetSecond.y,edge.targetSecond.z);
            glVertex3f(edge.second.x,edge.second.y,edge.second.z);glVertex3f(edge.targetFirst.x,edge.targetFirst.y,edge.targetFirst.z);glEnd();
        }
    }
    glPopMatrix();glPopAttrib();
}
}
