/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2014, ISR University of Coimbra.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the ISR University of Coimbra nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: David Portugal (2011-2014), and Luca Iocchi (2014)
*********************************************************************/

#include <sstream>
#include <string>
#include <ros/ros.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <nav_msgs/Odometry.h>



#include "PatrolAgent.h"

using namespace std;

void PatrolAgent::init(int argc, char** argv) {
    /*
            argv[0]=/.../patrolling_sim/bin/GBS
            argv[1]=__name:=XXXXXX
            argv[2]=grid
            argv[3]=ID_ROBOT
        */
    
    srand ( time(NULL) );
    
    //More than One robot (ID between 0 and 99)
    if ( atoi(argv[3])>NUM_MAX_ROBOTS || atoi(argv[3])<-1 ){
        ROS_INFO("The Robot's ID must be an integer number between 0 an 99"); //max 100 robots
        return;
    }else{
        ID_ROBOT = atoi(argv[3]);
        //printf("ID_ROBOT = %d\n",ID_ROBOT); //-1 in the case there is only 1 robot.
    }

    mapname = string(argv[2]);
    graph_file = "maps/"+mapname+"/"+mapname+".graph";
    
    //Check Graph Dimension:
    dimension = GetGraphDimension(graph_file.c_str());
    
    //Create Structure to save the Graph Info;
    vertex_web = new vertex[dimension];
    
    //Get the Graph info from the Graph File
    GetGraphInfo(vertex_web, dimension, graph_file.c_str());

#if 0
    /* Output Graph Data */
    for (i=0;i<dimension;i++){
        printf ("ID= %u\n", vertex_web[i].id);
        printf ("X= %f, Y= %f\n", vertex_web[i].x, vertex_web[i].y);
        printf ("#Neigh= %u\n", vertex_web[i].num_neigh);
        
        for (j=0;j<vertex_web[i].num_neigh; j++){
            printf("\tID = %u, DIR = %s, COST = %u\n", vertex_web[i].id_neigh[j], vertex_web[i].dir[j], vertex_web[i].cost[j]);
        }
        
        printf("\n");
    }
#endif

    interference = false;
    ResendGoal = false;
    goal_complete = true;
    last_interference = 0;
    goal_canceled_by_user = false;
    aborted_count = 0;
    resend_goal_count = 0;
    communication_delay = 0.0;
    lost_message_rate = 0.0;
    lastXpose=0;
    lastYpose=0;
    /* Define Starting Vertex/Position (Launch File Parameters) */

    ros::init(argc, argv, "patrol_agent");  // will be replaced by __name:=XXXXXX
    ros::NodeHandle nh;
    
    // wait a random time (avoid conflicts with other robots starting at the same time...)
    double r = 3.0 * ((rand() % 1000)/1000.0);
    ros::Duration wait(r); // seconds
    wait.sleep();
    
    double initial_x, initial_y;
    
    XmlRpc::XmlRpcValue list;
    nh.getParam("initial_pos", list);
    ROS_ASSERT(list.getType() == XmlRpc::XmlRpcValue::TypeArray);
    
    int value = ID_ROBOT;
    if (value == -1){value = 0;}
    
    ROS_ASSERT(list[2*value].getType() == XmlRpc::XmlRpcValue::TypeDouble);
    initial_x = static_cast<double>(list[2*value]);
    
    ROS_ASSERT(list[2*value+1].getType() == XmlRpc::XmlRpcValue::TypeDouble);
    initial_y = static_cast<double>(list[2*value+1]);
    
    //   printf("initial position: x = %f, y = %f\n", initial_x, initial_y);
    current_vertex = IdentifyVertex(vertex_web, dimension, initial_x, initial_y);
    //   printf("initial vertex = %d\n\n",current_vertex);
    
    
    //instantaneous idleness and last visit initialized with zeros:
    instantaneous_idleness = new double[dimension];
    last_visit = new double[dimension];
    for(size_t i=0; i<dimension; i++){
        instantaneous_idleness[i]= 0.0;
        last_visit[i]= 0.0;
        
        if (i==current_vertex){
            last_visit[i]= 0.1; //Avoids getting back at the initial vertex
        }
        //ROS_INFO("last_visit[%d]=%f", i, last_visit[i]);
    }

    //Publicar dados de "odom" para nó de posições
    //positions_pub = nh.advertise<nav_msgs::Odometry>("positions", 1); //only concerned about the most recent

    //Subscrever posições de outros robots
    //positions_sub = nh.subscribe<nav_msgs::Odometry>("positions", 10, boost::bind(&PatrolAgent::positionsCB, this, _1));
    
    char string1[40];
    char string2[40];
    
    if(ID_ROBOT==-1){
        strcpy (string1,"odom"); //string = "odom"
        strcpy (string2,"cmd_vel"); //string = "cmd_vel"
        TEAMSIZE = 1;
    }else{
        sprintf(string1,"robot_%d/odom",ID_ROBOT);
        sprintf(string2,"robot_%d/cmd_vel",ID_ROBOT);
        TEAMSIZE = ID_ROBOT + 1;
    }

    /* Set up listener for global coordinates of robots */
    listener = new tf::TransformListener();

    //Cmd_vel to backup:
    cmd_vel_pub  = nh.advertise<geometry_msgs::Twist>(string2, 1);
    
    //Subscrever para obter dados de "odom" do robot corrente
    odom_sub = nh.subscribe<nav_msgs::Odometry>(string1, 1, boost::bind(&PatrolAgent::odomCB, this, _1)); //size of the buffer = 1 (?)
    
    ros::spinOnce();
    
    //Publicar dados para "results"
    //results_pub = nh.advertise<std_msgs::Int16MultiArray>("results", 100);
    // results_sub = nh.subscribe("results", 10, resultsCB); //Subscrever "results" vindo dos robots
    //results_sub = nh.subscribe<std_msgs::Int16MultiArray>("results", 100, boost::bind(&PatrolAgent::resultsCB, this, _1) ); //Subscrever "results" vindo dos robots
    rcom_pub =nh.advertise<tcp_interface::RCOMMessage>("RCOMMessage",100);
    rcom_sub =nh.subscribe<tcp_interface::RCOMMessage>("RCOMMessage",100, boost::bind(&PatrolAgent::resultsCB, this, _1) );
    // last time comm delay has been applied
    last_communication_delay_time = ros::Time::now().toSec();

    readParams();
}

void PatrolAgent::ready() {
    
    char string1[40];
    
    /* Define Goal */
    if(ID_ROBOT==-1){
        strcpy (string1,"move_base"); //string = "move_base
    }else{
        sprintf(string1,"robot_%d/move_base",ID_ROBOT);
    }
    
    //printf("string = %s\n",string);
    ac = new MoveBaseClient(string1, true);
    
    //wait for the action server to come up
    while(!ac->waitForServer(ros::Duration(5.0))){
        ROS_INFO("Waiting for the move_base action server to come up");
    }
    ROS_INFO("Connected with move_base action server");
    
    // initialize_node(); //dizer q está vivo
    
    ros::Rate loop_rate(1); //1 segundo
    
    /* Wait until all nodes are ready.. */
    while (initialize){
        send_initialize_msg();
        ros::spinOnce();
        loop_rate.sleep();
    }

}


void PatrolAgent::readParams() {

    if (! ros::param::get("/goal_reached_wait", goal_reached_wait)) {
        goal_reached_wait = 0.0;
        ros::param::set("/goal_reached_wait", goal_reached_wait);
    }

    if (! ros::param::get("/communication_delay", communication_delay)) {
        communication_delay = 0.0;
        ros::param::set("/communication_delay", communication_delay);
    }

    if (! ros::param::get("/lost_message_rate", lost_message_rate)) {
        lost_message_rate = 0.0;
        ros::param::set("/lost_message_rate", lost_message_rate);
    }

}

void PatrolAgent::run() {
    
    // get ready
    ready();
    
    // Asynch spinner (non-blocking)
    ros::AsyncSpinner spinner(2); // Use n threads
    spinner.start();
    //     ros::waitForShutdown();

    /* Run Algorithm */
    
    ros::Rate loop_rate(30); //0.033 seconds or 30Hz
    
    while(ros::ok()){
        
        if (goal_complete) {
            onGoalComplete();  // can be redefined
            resend_goal_count=0;
        }
        else { // goal not complete (active)
            if (interference) {
                do_interference_behavior();
            }
            
            if (ResendGoal) {
                //Send the goal to the robot (Global Map)
                if (resend_goal_count<3) {
                    resend_goal_count++;
                    ROS_INFO("Re-Sending goal (%d) - Vertex %d (%f,%f)", resend_goal_count, next_vertex, vertex_web[next_vertex].x, vertex_web[next_vertex].y);
                    sendGoal(next_vertex);
                }
                else {
                    resend_goal_count=0;
                    onGoalNotComplete();
                }
                ResendGoal = false; //para nao voltar a entrar (envia goal so uma vez)
            }
            
            processEvents();
            
            if (end_simulation) {
                return;
            }

        }
        
        //ros::Duration delay = ros::Duration(0.1);
        //delay.sleep();
        
        // ros::spinOnce();
        loop_rate.sleep(); //David Portugal: I believe this is more correct to guarantee that the loop takes exactly 0.1 secs

    } // while ros.ok
}


void PatrolAgent::onGoalComplete()
{
    if(next_vertex>-1) {
        //Update Idleness Table:
        update_idleness();
        current_vertex = next_vertex;
    }
    
    //devolver proximo vertex tendo em conta apenas as idlenesses;
    next_vertex = compute_next_vertex();
    //printf("Move Robot to Vertex %d (%f,%f)\n", next_vertex, vertex_web[next_vertex].x, vertex_web[next_vertex].y);

    /** SEND GOAL (REACHED) AND INTENTION **/
    send_goal_reached(); // Send TARGET to monitor
    send_results();  // Algorithm specific function

    //Send the goal to the robot (Global Map)
    ROS_INFO("Sending goal - Vertex %d (%f,%f)\n", next_vertex, vertex_web[next_vertex].x, vertex_web[next_vertex].y);
    //sendGoal(vertex_web[next_vertex].x, vertex_web[next_vertex].y);
    sendGoal(next_vertex);  // send to move_base
    
    goal_complete = false;
}

void PatrolAgent::onGoalNotComplete()
{   
    int prev_vertex = next_vertex;
    
    ROS_INFO("Goal not complete - Vertex %d\n", next_vertex);
    
    //devolver proximo vertex tendo em conta apenas as idlenesses;
    next_vertex = compute_next_vertex();
    //printf("Move Robot to Vertex %d (%f,%f)\n", next_vertex, vertex_web[next_vertex].x, vertex_web[next_vertex].y);

    // Look for a random adjacent vertex different from the previous one
    int random_cnt=0;
    while (next_vertex == prev_vertex && random_cnt++<10) {
        int num_neighs = vertex_web[current_vertex].num_neigh;
        int i = rand() % num_neighs;
        next_vertex = vertex_web[current_vertex].id_neigh[i];
        ROS_INFO("Choosing another random vertex %d\n", next_vertex);
    }
    
    // Look for any random vertex different from the previous one
    while (next_vertex == prev_vertex) {
        int i = rand() % dimension;
        next_vertex = i;
        ROS_INFO("Choosing another random vertex %d\n", next_vertex);
    }

    //Send the goal to the robot (Global Map)
    ROS_INFO("Re-Sending NEW goal - Vertex %d (%f,%f)\n", next_vertex, vertex_web[next_vertex].x, vertex_web[next_vertex].y);
    //sendGoal(vertex_web[next_vertex].x, vertex_web[next_vertex].y);
    sendGoal(next_vertex);  // send to move_base
    
    goal_complete = false;
}


void PatrolAgent::processEvents() {
    
}

void PatrolAgent::update_idleness() {
    double now = ros::Time::now().toSec();

    for(size_t i=0; i<dimension; i++){
        if ((int)i == next_vertex){
            last_visit[i] = now;
        }
        instantaneous_idleness[i] = now - last_visit[i];

        //Show Idleness Table:
        //ROS_INFO("idleness[%u] = %f",i,instantaneous_idleness[i]);
    }
}

void PatrolAgent::send_initialize_msg (){ //ID,msg_type,1

    ROS_INFO("Initialize Node: Robot %d",ID_ROBOT);
    
    std_msgs::Int16MultiArray msg;
    msg.data.clear();
    msg.data.push_back(ID_ROBOT);
    msg.data.push_back(INITIALIZE_MSG_TYPE);
    msg.data.push_back(1);  // Robot initialized
    
    int count = 0;
    
    //ATENÇÃO ao PUBLICADOR!
    ros::Rate loop_rate(1); //meio segundo
    
    //while (true){ //send activation msg forever
        //results_pub.publish(msg);
        //ROS_INFO("publiquei msg: %s\n", msg.data.c_str());
        //ros::spinOnce();
        do_send_message(msg);
        loop_rate.sleep();
        count++;
    //}
}

void PatrolAgent::getRobotPose(int robotid, float &x, float &y, float &theta) {
    
    if (listener==NULL) {
        ROS_ERROR("TF listener null");
        return;
    }
    
    std::stringstream ss; ss << "robot_" << robotid;
    std::string robotname = ss.str();
    std::string sframe = "/map";                //Patch David Portugal: Remember that the global map frame is "/map"
    std::string dframe = "/" + robotname + "/base_link";
    tf::StampedTransform transform;

    try {
        listener->waitForTransform(sframe, dframe, ros::Time(0), ros::Duration(3));
        listener->lookupTransform(sframe, dframe, ros::Time(0), transform);
    }
    catch(tf::TransformException ex) {
        ROS_ERROR("Cannot transform from %s to %s\n",sframe.c_str(),dframe.c_str());
        ROS_ERROR("%s", ex.what());
    }

    x = transform.getOrigin().x();
    y = transform.getOrigin().y();
    theta = tf::getYaw(transform.getRotation());
    // printf("Robot %d pose : %.1f %.1f \n",robotid,x,y);
}

void PatrolAgent::odomCB(const nav_msgs::Odometry::ConstPtr& msg) { //colocar propria posicao na tabela
    
    //  printf("Colocar Propria posição na tabela, ID_ROBOT = %d\n",ID_ROBOT);
    int idx = ID_ROBOT;
    
    if (ID_ROBOT<=-1){
        idx = 0;
    }
    
    float x,y,th;
    getRobotPose(idx,x,y,th);
    
    xPos[idx]=x; // msg->pose.pose.position.x;
    yPos[idx]=y; // msg->pose.pose.position.y;
    //printf(" POSITION RECEIVED x:%f y:%f \n",xPos[idx],yPos[idx]);
    //  printf("Posicao colocada em Pos[%d]\n",idx);
}



void PatrolAgent::sendGoal(int next_vertex) 
{
    goal_canceled_by_user = false;
    
    double target_x = vertex_web[next_vertex].x,
            target_y = vertex_web[next_vertex].y;
    
    //Define Goal:
    move_base_msgs::MoveBaseGoal goal;
    //Send the goal to the robot (Global Map)
    geometry_msgs::Quaternion angle_quat = tf::createQuaternionMsgFromYaw(0.0);
    goal.target_pose.header.frame_id = "map";
    goal.target_pose.header.stamp = ros::Time::now();
    goal.target_pose.pose.position.x = target_x; // vertex_web[current_vertex].x;
    goal.target_pose.pose.position.y = target_y; // vertex_web[current_vertex].y;
    goal.target_pose.pose.orientation = angle_quat; //alpha -> orientação  (queria optimizar este parametro -> através da direcção do vizinho!)
    ac->sendGoal(goal, boost::bind(&PatrolAgent::goalDoneCallback, this, _1, _2), boost::bind(&PatrolAgent::goalActiveCallback,this), boost::bind(&PatrolAgent::goalFeedbackCallback, this,_1));
}

void PatrolAgent::cancelGoal() 
{
    goal_canceled_by_user = true;
    ac->cancelAllGoals();
}


void PatrolAgent::goalDoneCallback(const actionlib::SimpleClientGoalState &state, const move_base_msgs::MoveBaseResultConstPtr &result){ //goal terminado (completo ou cancelado)
    //  ROS_INFO("Goal is complete (suceeded, aborted or cancelled).");
    // If the goal succeeded send a new one!
    //if(state.state_ == actionlib::SimpleClientGoalState::SUCCEEDED) sendNewGoal = true;
    // If it was aborted time to back up!
    //if(state.state_ == actionlib::SimpleClientGoalState::ABORTED) needToBackUp = true;
    
    if(state.state_ == actionlib::SimpleClientGoalState::SUCCEEDED){
        ROS_INFO("Goal reached ... WAITING %.2f sec",goal_reached_wait);
        ros::Duration delay(goal_reached_wait); // wait after goal is reached
        delay.sleep();
        ROS_INFO("Goal reached ... DONE");
        goal_complete = true;
    }else{
        aborted_count++;
        ROS_INFO("CANCELLED or ABORTED... %d",aborted_count);   //tentar voltar a enviar goal..
        //backUpCounter = 0;
        //backup();
        if (!goal_canceled_by_user) {
            ROS_INFO("Not cancelled by the intereference... Resend Goal!");
            ResendGoal = true;
        }
    }
}

void PatrolAgent::goalActiveCallback(){  //enquanto o robot esta a andar para o goal...
    goal_complete = false;
    //      ROS_INFO("Goal is active.");
}

void PatrolAgent::goalFeedbackCallback(const move_base_msgs::MoveBaseFeedbackConstPtr &feedback){    //publicar posições

    send_positions();
    
    interference = check_interference(ID_ROBOT);
}

void PatrolAgent::send_goal_reached() {
    // [ID,msg_type,vertex,intention,0]
    std_msgs::Int16MultiArray msg;
    msg.data.clear();
    msg.data.push_back(ID_ROBOT);
    msg.data.push_back(TARGET_REACHED_MSG_TYPE);
    msg.data.push_back(current_vertex);
    //msg.data.push_back(next_vertex);
    //msg.data.push_back(0); //David Portugal: is this necessary?
    printf("Send goal reached message: %d %d\n",ID_ROBOT,current_vertex);
    
    do_send_message(msg);
    //results_pub.publish(msg);
    //ros::spinOnce();
}

bool PatrolAgent::check_interference (int ID_ROBOT){ //verificar se os robots estao proximos
    
    int i;
    double dist_quad;
    
    if (ros::Time::now().toSec()-last_interference<10)  // seconds
        return false; // false if within 10 seconds from the last one
    
    /* Poderei usar TEAMSIZE para afinar */
    for (i=0; i<ID_ROBOT; i++){ //percorrer vizinhos (assim asseguro q cada interferencia é so encontrada 1 vez)
        
        dist_quad = (xPos[i] - xPos[ID_ROBOT])*(xPos[i] - xPos[ID_ROBOT]) + (yPos[i] - yPos[ID_ROBOT])*(yPos[i] - yPos[ID_ROBOT]);
        
        if (dist_quad <= /*sqrt*/4){    //robots are 2 meter or less apart
            //          ROS_INFO("Feedback: Robots are very close. INTERFERENCE! Dist_Quad = %f", dist_quad);
            last_interference = ros::Time::now().toSec();
            return true;
        }
    }
    return false;
    
}

void PatrolAgent::backup(){
    
    while (backUpCounter<=100){

        if(backUpCounter==0){
            ROS_INFO("The wall is too close! I need to do some backing up...");
            // Move the robot back...
            geometry_msgs::Twist cmd_vel;
            cmd_vel.linear.x = -0.1;
            cmd_vel.angular.z = 0.0;
            cmd_vel_pub.publish(cmd_vel);
        }

        if(backUpCounter==40){
            // Turn the robot around...
            geometry_msgs::Twist cmd_vel;
            cmd_vel.linear.x = 0.0;
            cmd_vel.angular.z = 0.5;
            cmd_vel_pub.publish(cmd_vel);
        }

        if(backUpCounter==100){
            // Stop the robot...
            geometry_msgs::Twist cmd_vel;
            cmd_vel.linear.x = 0.0;
            cmd_vel.angular.z = 0.0;
            cmd_vel_pub.publish(cmd_vel);
            
            ROS_INFO("Done backing up, now on with my life!");
        }

        backUpCounter++;
    }
    
}

void PatrolAgent::do_interference_behavior()
{
    ROS_INFO("Interference detected! Executing interference behavior...\n");
    send_interference();  // send interference to monitor for counting
    
#if 1
    // Stop the robot..
    cancelGoal();
    ROS_INFO("Robot stopped");
    ros::Duration delay(3); // seconds
    delay.sleep();
    ResendGoal = true;
#else    
    //get own "odom" positions...
    ros::spinOnce();

    //Waiting until conflict is solved...
    while(interference){
        interference = check_interference(ID_ROBOT);
        if (goal_complete || ResendGoal){
            interference = false;
        }
    }
#endif
}




// ROBOT-ROBOT COMMUNICATION



void PatrolAgent::send_positions()
{
    //Publish Position to common node:
    //nav_msgs::Odometry msg;
    /*
    int idx = ID_ROBOT;

     if (ID_ROBOT <= -1){
        msg.header.frame_id = "map";    //identificador do robot q publicou
        idx = 0;
    }else{
        char string[20];
        sprintf(string,"robot_%d/map",ID_ROBOT);
        msg.header.frame_id = string;
    }

    msg.pose.pose.position.x = xPos[idx]; //send odometry.x
    msg.pose.pose.position.y = yPos[idx]; //send odometry.y

    positions_pub.publish(msg);
    */
    
        lastXpose=xPos[ID_ROBOT]*1000;
		lastYpose=yPos[ID_ROBOT]*1000;
		std_msgs::Int16MultiArray msg;
		msg.data.clear();
		msg.data.push_back(ID_ROBOT);
		msg.data.push_back(POSITION_MSG_TYPE);
		msg.data.push_back(lastXpose);
		msg.data.push_back(lastYpose);
		//msg.data.push_back(next_vertex);
		//msg.data.push_back(0); //David Portugal: is this necessary?
		//printf(" POSITION TO SEND x:%f y:%f \n",lastXpose,lastYpose);
		do_send_message(msg);
    
    //results_pub.publish(msg);
    //ros::spinOnce();


    ros::spinOnce();
}


void PatrolAgent::receive_positions()
{
    
}
/*
void PatrolAgent::positionsCB(const nav_msgs::Odometry::ConstPtr& msg) { //construir tabelas de posições

    //     printf("Construir tabela de posicoes (receber posicoes), ID_ROBOT = %d\n",ID_ROBOT);

    char id[20]; //identificador do robot q enviou a msg d posição...
    strcpy( id, msg->header.frame_id.c_str() );
    //int stamp = msg->header.seq;
    //     printf("robot q mandou msg = %s\n", id);
    
    // Build Positions Table
    
    if (ID_ROBOT>-1){
        //verify id "XX" of robot: (string: "robot_XX/map")

        char str_idx[4];
        uint i;
        
        for (i=6; i<10; i++){
            if (id[i]=='/'){
                str_idx[i-6] = '\0';
                break;
            }else{
                str_idx[i-6] = id[i];
            }
        }
        
        int idx = atoi (str_idx);
        //  printf("id robot q mandou msg = %d\n",idx);
        
        if (idx >= TEAMSIZE && TEAMSIZE <= NUM_MAX_ROBOTS){
            //update teamsize:
            TEAMSIZE = idx+1;
        }
        
        if (ID_ROBOT != idx){  //Ignore own positions
            xPos[idx]=msg->pose.pose.position.x;
            yPos[idx]=msg->pose.pose.position.y;
        }
        //      printf ("Position Table:\n frame.id = %s\n id_robot = %d\n xPos[%d] = %f\n yPos[%d] = %f\n\n", id, idx, idx, xPos[idx], idx, yPos[idx] );
    }
    
    receive_positions();
}
*/

void PatrolAgent::send_results() { 

}

// simulates blocking send operation with delay in communication
void PatrolAgent::do_send_message(std_msgs::Int16MultiArray &msg) {
    if (communication_delay>0.001) {
        //double current_time = ros::Time::now().toSec();
        //if (current_time-last_communication_delay_time>1.0) {
        //ROS_INFO("Communication delay %.1f",communication_delay);
        ros::Duration delay(communication_delay); // seconds
        delay.sleep();
        //last_communication_delay_time = current_time;
        //}
    }
    

    
    std::stringstream ss;

    for (std::vector<signed short>::iterator it = msg.data.begin(); it != msg.data.end(); ++it) {
        ss << *it <<" ";
    }
    std::string s = ss.str();

    //results_pub.publish(msg);
    tcp_interface::RCOMMessage m;
    m.header.stamp = ros::Time::now();
    m.robotreceiver="all";
    std::stringstream robotname; robotname << "robot_" << ID_ROBOT;
    m.robotsender=robotname.str();
    m.value=s;
    rcom_pub.publish(m);
    ros::spinOnce();
}


void PatrolAgent::receive_results() {

}

void PatrolAgent::send_interference(){
    //interference: [ID,msg_type]

    printf("Send Interference: Robot %d\n",ID_ROBOT);
    
    std_msgs::Int16MultiArray msg;
    msg.data.clear();
    msg.data.push_back(ID_ROBOT);
    msg.data.push_back(INTERFERENCE_MSG_TYPE);
    
    do_send_message(msg);
    //results_pub.publish(msg);
    //ros::spinOnce();
}




void PatrolAgent::resultsCB(const tcp_interface::RCOMMessage::ConstPtr& msg) {
    
    //std::vector<signed short>::const_iterator it = msg->data.begin();
    
    vresults.clear();
    
    //for (size_t k=0; k<msg->data.size(); k++) {
    //    vresults.push_back(*it); it++;
    //}
    
    signed short buf;

    string message;
    message = msg->value;
    stringstream ss(message); // Insert the string into a stream

    //printf(" MESSAGE RECEIVED %s \n",message.c_str());
    while (ss >> buf){
        //tokens.push_back(buf);
        vresults.push_back(buf);
    }



    int id_sender = vresults[0];
    int msg_type = vresults[1];

    std::stringstream robotname; robotname << "robot_" << ID_ROBOT;
    
    if (msg->robotreceiver!=robotname.str()) //&& msg->robotreceiver!="all")
	return;
    
    //printf("--> MESSAGE FROM %d To %s/%s TYPE %d ...\n",id_sender, 
    //   msg->robotreceiver.c_str(),robotname.str().c_str(), msg_type);

    
    
    
    if(id_sender!=ID_ROBOT && msg_type==POSITION_MSG_TYPE){
        if (id_sender >= TEAMSIZE && TEAMSIZE < NUM_MAX_ROBOTS){
            //update teamsize:
            TEAMSIZE = id_sender+1;
        }
        //printf(" POSITION RECEIVED in MM FROM %d x:%d y:%d \n",id_sender,vresults[2],vresults[3]);
        xPos[id_sender]=float(vresults[2])/1000;
        yPos[id_sender]=float(vresults[3])/1000;
        //printf(" POSITION RECEIVED FROM %d x:%f y:%f \n",id_sender,xPos[id_sender],yPos[id_sender]);
        receive_positions();

    }else{

        // messages coming from the monitor
        if (id_sender==-1 && msg_type==INITIALIZE_MSG_TYPE) {
            if (initialize==true && vresults[2]==100) {   //"-1,msg_type,100" (BEGINNING)
                ROS_INFO("Let's Patrol!\n");
                double r = 1.0 * ((rand() % 1000)/1000.0);
                ros::Duration wait(r); // seconds
                wait.sleep();
                initialize = false;
            }

            if (initialize==false && vresults[2]==999) {   //"-1,msg_type,999" (END)
                ROS_INFO("The simulation is over. Let's leave");
                end_simulation = true;
            }
        }

        if (!initialize) {
#if 0
            // communication delay
            if ((communication_delay>0.001) && (id_sender!=ID_ROBOT)) {
                double current_time = ros::Time::now().toSec();
                if (current_time-last_communication_delay_time>1.0) {
                    ROS_INFO("Communication delay %.1f",communication_delay);
                    ros::Duration delay(communication_delay); // seconds
                    delay.sleep();
                    last_communication_delay_time = current_time;
                }
            }
            bool lost_message = false;
            if ((lost_message_rate>0.0001)&& (id_sender!=ID_ROBOT)) {
                double r = (rand() % 1000)/1000.0;
                lost_message = r < lost_message_rate;
            }
            if (lost_message) {
                ROS_INFO("Lost message");
            }
            else
#endif
                receive_results();
        }
    }
    ros::spinOnce();

}
