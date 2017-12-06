#include <cnoid/SimpleController>
#include <cnoid/Joystick>

using namespace std;
using namespace cnoid;

class DoubleArmV7Controller : public cnoid::SimpleController
{
    Body* body;
    double dt;

    vector<int> armJointIdMap;
    vector<Link*> armJoints;
    vector<double> qref;
    vector<double> qold;
    vector<double> pgain;
    vector<double> dgain;
    double trackgain;

    Link::ActuationMode mainActuationMode;

    enum AxisType { STICK, BUTTON };

    struct OperationAxis {
        Link* joint;
        AxisType type;
        int id;
        double ratio;
        double offset;
    };

    vector<vector<OperationAxis>> operationAxes;
    int operationSetIndex;
    bool prevSelectButtonState;

    const int SHIFT_BUTTON_ID = 5;
    const int SELECT_BUTTON_ID = 10;
    
    Link* trackL;
    Link* trackR;
    bool hasPseudoContinuousTracks;
    bool hasContinuousTracks;

    Joystick joystick;

    Link* link(const char* name) { return body->link(name); }

public:

    DoubleArmV7Controller();
    virtual bool initialize(SimpleControllerIO* io) override;
    bool initPseudoContinuousTracks(SimpleControllerIO* io);
    bool initContinuousTracks(SimpleControllerIO* io);
    void initArms(SimpleControllerIO* io);
    void initPDGain();
    void initJoystickKeyBind();
    virtual bool control() override;
    void controlTracks();
    void setTargetArmPositions();
    void controlArms();
    void controlArmsWithTorque();
    void controlArmsWithVelocity();
    void controlArmsWithPosition();
};

DoubleArmV7Controller::DoubleArmV7Controller()
{
    mainActuationMode = Link::ActuationMode::JOINT_TORQUE;
    hasPseudoContinuousTracks = false;
    hasContinuousTracks = false;
}

bool DoubleArmV7Controller::initialize(SimpleControllerIO* io)
{
    body = io->body();
    dt = io->timeStep();

    string option = io->optionString();
    if(option == "velocity")        mainActuationMode = Link::ActuationMode::JOINT_VELOCITY;
    else if(option  == "position")  mainActuationMode = Link::ActuationMode::JOINT_ANGLE;
    else                            mainActuationMode = Link::ActuationMode::JOINT_TORQUE;


    if(!initPseudoContinuousTracks(io))
        initContinuousTracks(io);
    initArms(io);
    initPDGain();
    initJoystickKeyBind();
    return true;
}

bool DoubleArmV7Controller::initPseudoContinuousTracks(SimpleControllerIO* io)
{
    trackL = body->link("TRACK_L");
    trackR = body->link("TRACK_R");
    if(!trackL) return false;
    if(!trackR) return false;

    if(trackL->actuationMode() == Link::JOINT_SURFACE_VELOCITY &&
    trackR->actuationMode() == Link::JOINT_SURFACE_VELOCITY   ){
        io->enableOutput(trackL);
        io->enableOutput(trackR);
        hasPseudoContinuousTracks = true;
        return true;
    }
    return false;
}

bool DoubleArmV7Controller::initContinuousTracks(SimpleControllerIO* io)
{
    trackL = body->link("WHEEL_L0");
    trackR = body->link("WHEEL_R0");
    if(!trackL) return false;
    if(!trackR) return false;

    if(mainActuationMode == Link::ActuationMode::JOINT_TORQUE){
        trackL->setActuationMode(Link::ActuationMode::JOINT_TORQUE);
        trackR->setActuationMode(Link::ActuationMode::JOINT_TORQUE);
    }else{
        trackL->setActuationMode(Link::ActuationMode::JOINT_VELOCITY);
        trackR->setActuationMode(Link::ActuationMode::JOINT_VELOCITY);
    }
    io->enableOutput(trackL);
    io->enableOutput(trackR);
    hasContinuousTracks = true;
    return true;
}

#include <iostream>
void DoubleArmV7Controller::initArms(SimpleControllerIO* io)
{
    for(auto joint : body->joints()){
        if(joint->jointId() >= 0 && (joint->isRevoluteJoint() || joint->isPrismaticJoint())){
            joint->setActuationMode(mainActuationMode);
            io->enableIO(joint);
            armJointIdMap.push_back(armJoints.size());
            armJoints.push_back(joint);
            qref.push_back(joint->q());
        } else {
            armJointIdMap.push_back(-1);
        }
    }
    qold = qref;
}

void DoubleArmV7Controller::initPDGain()
{
    // Tracks
    if(hasPseudoContinuousTracks) trackgain = 1.0;
    if(hasContinuousTracks){
        if(mainActuationMode == Link::ActuationMode::JOINT_TORQUE){
            trackgain = 2000.0;
        }else{
            trackgain = 2.0;
        }
    }

    // Arm
    if(mainActuationMode == Link::ActuationMode::JOINT_TORQUE){
        pgain = {
        /* MFRAME */ 200000, /* BLOCK */ 150000, /* BOOM */ 150000, /* ARM  */ 100000,
        /* PITCH  */  30000, /* ROLL  */  20000, /* TIP1 */    500, /* TIP2 */    500,
        /* UFRAME */ 150000, /* SWING */  50000, /* BOOM */ 100000, /* ARM  */  80000,
        /* ELBOW */   30000, /* YAW   */  20000, /* HAND */    500, /* ROD  */  50000};
        dgain = {
        /* MFRAME */ 20000, /* BLOCK */ 15000, /* BOOM */ 10000, /* ARM  */ 5000,
        /* PITCH  */   500, /* ROLL  */   500, /* TIP1 */    50, /* TIP2 */   50,
        /* UFRAME */ 15000, /* SWING */  1000, /* BOOM */  3000, /* ARM  */ 2000,
        /* ELBOW */    500, /* YAW   */   500, /* HAND */    20, /* ROD  */ 5000};
    }
    if(mainActuationMode == Link::ActuationMode::JOINT_VELOCITY){
        pgain = {
        /* MFRAME */ 100, /* BLOCK */ 180, /* BOOM */ 150, /* ARM  */ 100,
        /* PITCH  */  30, /* ROLL  */  20, /* TIP1 */   5, /* TIP2 */   5,
        /* UFRAME */ 150, /* SWING */ 180, /* BOOM */ 100, /* ARM  */  80,
        /* ELBOW */   30, /* YAW   */  20, /* HAND */   1, /* ROD  */  50};
    }
    if(mainActuationMode == Link::ActuationMode::JOINT_ANGLE){
    }
}

void DoubleArmV7Controller::initJoystickKeyBind()
{
    operationAxes = {
    {
    { link("MFRAME"),       STICK,  0, -1.0, 0.0 },
    { link("BLOCK"),        STICK,  2, -1.0, 0.0 },
    { link("BOOM"),         STICK,  1, -1.0, 0.0 },
    { link("ARM"),          STICK,  3,  1.0, 0.0 },
    { link("TOHKU_PITCH"),  STICK,  5,  1.0, 0.0 },
    { link("TOHKU_ROLL"),   STICK,  6,  1.0, 1.0 },
    { link("TOHKU_ROLL"),   STICK,  7, -1.0, 1.0 },
    { link("TOHKU_TIP_01"), BUTTON, 1,  1.0, 0.0 },
    { link("TOHKU_TIP_02"), BUTTON, 1,  1.0, 0.0 },
    { link("TOHKU_TIP_01"), BUTTON, 2, -1.0, 0.0 },
    { link("TOHKU_TIP_02"), BUTTON, 2, -1.0, 0.0 } },
    {
    { link("UFRAME"),       STICK,  0, -1.0, 0.0 },
    { link("MNP_SWING"),    STICK,  2, -1.0, 0.0 },
    { link("MANIBOOM"),     STICK,  1, -1.0, 0.0 },
    { link("MANIARM"),      STICK,  3,  1.0, 0.0 },
    { link("MANIELBOW"),    STICK,  5,  1.0, 0.0 },
    { link("YAWJOINT"),     STICK,  4, -1.0, 0.0 },
    { link("HANDBASE"),     STICK,  6, -1.0, 1.0 },
    { link("HANDBASE"),     STICK,  7,  1.0, 1.0 },
    { link("PUSHROD"),      BUTTON, 1,  0.1, 0.0 },
    { link("PUSHROD"),      BUTTON, 2, -0.1, 0.0 } } };

    operationSetIndex = 0;
    prevSelectButtonState = false;
}

bool DoubleArmV7Controller::control()
{
    joystick.readCurrentState();

    bool selectButtonState = joystick.getButtonState(SELECT_BUTTON_ID);
    if(!prevSelectButtonState && selectButtonState){
        operationSetIndex = 1 - operationSetIndex;
    }
    prevSelectButtonState = selectButtonState;

    trackL->u() = 0.0;
    trackL->dq() = 0.0;
    trackR->u() = 0.0;
    trackR->dq() = 0.0;
    if(joystick.getButtonState(SHIFT_BUTTON_ID)){
        controlTracks();
    }else{
        setTargetArmPositions();
    }
    controlArms();

    return true;
}

void DoubleArmV7Controller::controlTracks()
{
    double pos[2];
    for(int i=0; i < 2; ++i){
        pos[i] = joystick.getPosition(i);
        if(fabs(pos[i]) < 0.2){
            pos[i] = 0.0;
        }
    }
    // set the velocity of each track
    if(hasPseudoContinuousTracks || mainActuationMode == Link::ActuationMode::JOINT_VELOCITY
        || mainActuationMode == Link::ActuationMode::JOINT_ANGLE){
        trackL->dq() = trackgain * (-2.0 * pos[1] + pos[0]);
        trackR->dq() = trackgain * (-2.0 * pos[1] - pos[0]);
    }else if(mainActuationMode == Link::ActuationMode::JOINT_TORQUE){
        trackL->u() = trackgain * (-2.0 * pos[1] + pos[0]);
        trackR->u() = trackgain * (-2.0 * pos[1] - pos[0]);
    }
}

void DoubleArmV7Controller::setTargetArmPositions()
{
    const vector<OperationAxis>& axes = operationAxes[operationSetIndex];

    for(auto& axis : axes){
        Link* joint = axis.joint;
        double& q = qref[armJointIdMap[joint->jointId()]];
        if(axis.type == BUTTON){
            if(joystick.getButtonState(axis.id)){
                q += axis.ratio * dt;
            }
        } else if(axis.type == STICK){
                double pos = joystick.getPosition(axis.id);
                pos += axis.offset;
                q += axis.ratio * pos * dt;
            }
        if(q > joint->q_upper()){
            q = joint->q_upper();
        } else if(q < joint->q_lower()){
                q = joint->q_lower();
            }
    }
}

void DoubleArmV7Controller::controlArms()
{
    if(mainActuationMode == Link::ActuationMode::JOINT_VELOCITY){
        controlArmsWithVelocity();
    }else if(mainActuationMode == Link::ActuationMode::JOINT_ANGLE){
        controlArmsWithPosition();
    }else{
        controlArmsWithTorque();
    }
}

void DoubleArmV7Controller::controlArmsWithTorque()
{
    for(size_t i=0; i < armJoints.size(); ++i){
        Link* joint = armJoints[i];
        double q = joint->q();
        double dq = (q - qold[i]) / dt;
        joint->u() = (qref[i] - q) * pgain[i] + (0.0 - dq) * dgain[i];
        qold[i] = q;
    }
}

void DoubleArmV7Controller::controlArmsWithVelocity()
{
    for(size_t i=0; i < armJoints.size(); ++i){
        Link* joint = armJoints[i];
        double q = joint->q();
        joint->dq() = (qref[i] - q) * pgain[i];
    }
}

void DoubleArmV7Controller::controlArmsWithPosition()
{
    for(size_t i=0; i < armJoints.size(); ++i){
        Link* joint = armJoints[i];
        joint->q() = qref[i];
    }
}


CNOID_IMPLEMENT_SIMPLE_CONTROLLER_FACTORY(DoubleArmV7Controller)
