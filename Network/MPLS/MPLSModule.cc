#include <omnetpp.h>
#include <string.h>
#include "MPLSModule.h"



Define_Module(MPLSModule);

void MPLSModule::initialize()
{
    ipdataQueue.setName("ipdataQueue");
    ldpQueue.setName("ldpQueue");
    myQueriesCache.setName("myQueriesCache");

    //Get the process delay setting
    delay1 = par("procdelay");     // FIXME currently not used

    //Is this LSR an Ingress Router
    isIR = par("isIR");

    //Is this ER an Egress Router
    isER = par("isER");

    //Which FEC classification scheme is used
    classifierType = par("classifier").longValue();

    //Signalling component is ready or not
    isSignallingReady =false;
}


void MPLSModule::handleMessage(cMessage *msg)
{
    if (!strcmp(msg->arrivalGate()->name(), "fromL3"))
    {
        ev << "Processing message from L3: " << msg << endl;
        processPacketFromL3(msg);
    }
    else if (!strcmp(msg->arrivalGate()->name(), "fromSignalModule"))
    {
        ev << "Processing message from signalling module: " << msg << endl;
        processPacketFromSignalling(msg);
    }
    else
    {
        ev << "Processing message from L2: " << msg << endl;
        processPacketFromL2(msg);
    }
}

void MPLSModule::processPacketFromL3(cMessage *msg)
{
    IPDatagram *ipdata = check_and_cast<IPDatagram *>(msg);
    int gateIndex = msg->arrivalGate()->index();

    // If the MPLS processing is not on, then simply passing the packet
    if (ipdata->hasPar("trans"))   // FIXME put this field into IPDatagram!
    {
       send(ipdata, "toL2", gateIndex);
       return;
    }

    // IP data from L3 and requires MPLS processing
    MPLSPacket *outPacket =new MPLSPacket();
    outPacket->encapsulate(ipdata);

    // This is native IP
    outPacket->pushLabel(-1);
    send(outPacket, "toL2", gateIndex);
}

void MPLSModule::processPacketFromSignalling(cMessage *msg)
{
    RoutingTable *rt = routingTableAccess.get();
    LIBTable *lt = libTableAccess.get();

    if (!isSignallingReady)
    {
        // This is message from LDP saying that it is ready.
        // FIXME some assert() to make sure it's really that?
        ev << "LDP says it is ready, sending out buffered LDP queries to it\n";

        isSignallingReady = true;

        // Start to send out all the pending queries to LDP
        cModule *ldpMod = parentModule()->submodule("signal_module"); // FIXME maybe use connections instead of direct sending....
        if (!ldpMod)  error("Cannot find signal_module");
        for (int i=0; i<ldpQueue.items(); i++)
        {
            cMessage *ldpMsg = (cMessage*) ldpQueue.remove(i);
            sendDirect(ldpMsg, 0.0, ldpMod, "from_mpls_switch");
        }
        delete msg;
        return;
    }

    ev << "Message from LDP\n";

    // Get the mapping from LDP daemon
    int label  = msg->par("label").longValue();
    int returnedFEC = (int)(msg->par("fec").longValue());
    bool isLDP = false;
    if (msg->hasPar("my_name"))
        isLDP = true;

    delete msg; // no needed any more

    std::vector<fec>::iterator iterF;
    fec iter;
    if (!isLDP)
    {
        for (int i=0;i< fecList.size();i++)
        {
            if (fecList[i].fecId ==(2*MAX_LSP_NO - returnedFEC))
            {
                fecList[i].fecId = returnedFEC;
                break;
            }
        }
    }

    // Update LSP id
    for (iterF = fecList.begin(); iterF!=fecList.end();iterF++)
    {
        iter = *iterF;
        ev << "LSPid=" << iter.fecId << "\n";
        ev << "dest=" << IPAddress(iter.dest).getString() << "\n";
        ev << "src =" << IPAddress(iter.src).getString() << "\n";
    }

    // Looks like we're sending out buffered IP datagrams and MPLS packets
    for (int i=0; i<ipdataQueue.items(); i++)
    {
        // Release packets in queue
        IPDatagram *data =  dynamic_cast<IPDatagram*>(ipdataQueue[i]);
        MPLSPacket *mplsPck =  dynamic_cast<MPLSPacket*>(ipdataQueue[i]);

        if (data==NULL && mplsPck ==NULL)
            continue;

        //Incoming interface
        int gateIndex;
        if (data!=NULL)
        {
            gateIndex =data->par("gateIndex");
        }
        else if (mplsPck!=NULL)
        {
            gateIndex =mplsPck->par("gateIndex");
            data =(IPDatagram *) (mplsPck->decapsulate());
        }

        InterfaceEntry* ientry= rt->getInterfaceByIndex(gateIndex);
        string senderInterface =string(ientry->name.c_str());
        int fecID = classifyPacket(data, classifierType);

        if (fecID ==returnedFEC)
        {
            //This only has the polishing purpose
            /*
            std::list<fec_color_mapping>::iterator iterF;
            fec_color_mapping iter;
            for (iterF = pathColor.begin();iterF != pathColor.end();iterF++)
            {
                iter =(fec_color_mapping)*iterF;
                if (iter.fecId == fecID)
                    break;
            }
            */

            // Construct a new MPLS packet
            MPLSPacket* newPacket = NULL;
            if (mplsPck !=NULL)
            {
                newPacket = mplsPck;
            }
            else
            {
                newPacket = new MPLSPacket();
                newPacket->encapsulate(check_and_cast<IPDatagram*>(ipdataQueue[i]));
            }
            newPacket->pushLabel(label);

            //if (iterF != pathColor.end()) (???)
            newPacket->setKind(fecID);

            // Find the outgoing interface
            string outgoingInterface = lt->requestOutgoingInterface(senderInterface, label);

            // A check routine will be added later to make sure outgoingInterface !="X"
            int outgoingPort =rt->findInterfaceByName(outgoingInterface.c_str());

            send(newPacket, "toL2", outgoingPort);

            // Remove the message out of the queue
            ipdataQueue.remove(i);
        }
    }
}


void MPLSModule::processPacketFromL2(cMessage *msg)
{
    RoutingTable *rt = routingTableAccess.get();
    LIBTable *lt = libTableAccess.get();
    cModule *ldpMod = parentModule()->submodule("signal_module"); // FIXME should use connections instead of direct sending....
    if (!ldpMod) error("Cannot find signal_module");

    IPDatagram *ipdata = dynamic_cast<IPDatagram *>(msg);
    MPLSPacket *mplsPacket = dynamic_cast<MPLSPacket *>(msg);

    int gateID=msg->arrivalGateId();
    int gateIndex=gateID-(gate("fromL2",0)->id());
    if (ipdata!=NULL && !isIR)
    {
        ipdata->addPar("trans")=0;
        send(ipdata, "toL3", gateIndex);
    }
    else if (mplsPacket!=NULL)
    {
        // (A) Here we process MPLS packets
        InterfaceEntry* ientry= rt->getInterfaceByIndex(gateIndex);
        string senderInterface =string(ientry->name.c_str());
        int oldLabel=  mplsPacket->getLabel();

        if (oldLabel !=-1) // This is not IP native packet
        {
            int newLabel = lt->requestNewLabel(senderInterface, oldLabel);
            int optCode  = lt->getOptCode(senderInterface, oldLabel);

            if (newLabel >=0) //New label can be found
            {
                switch(optCode)
                {
                    case PUSH_OPER:
                        mplsPacket->pushLabel(newLabel);
                        break;
                    case SWAP_OPER:
                        mplsPacket->swapLabel(newLabel);
                        break;
                    case POP_OPER:
                        mplsPacket->popLabel();
                        break;
                    default:
                        error("Unknown MPLS OptCode %d", optCode);
                }

                string outgoingInterface= lt->requestOutgoingInterface(senderInterface, newLabel);

                int outgoingPort =rt->findInterfaceByName(outgoingInterface.c_str());

                if (optCode == SWAP_OPER)
                    ev << " Swap label(" << oldLabel << ") with label(" << newLabel << ")\n";
                else if (optCode == PUSH_OPER)
                    ev << "Push label " << newLabel << "\n";
                else if (optCode == POP_OPER)
                    ev << "Pop label " << oldLabel << "\n";

                ev << " Sending the packet to interface "<< outgoingInterface.c_str() <<"\n";

                send(mplsPacket, "toL2", outgoingPort);

            }
            else if (isER && (newLabel==-1)) // ER router and the new label must be native IP
            {
                string outgoingInterface= lt->requestOutgoingInterface(senderInterface, newLabel, oldLabel);
                int outgoingPort =rt->findInterfaceByName(outgoingInterface.c_str());

                mplsPacket->popLabel();

                //Test if this is a tunnel ER
                if (mplsPacket->noLabel())
                {
                    IPDatagram *nativeIP= check_and_cast<IPDatagram*>(mplsPacket->decapsulate());
                    cMessage* dupIPDatagram = (cMessage *) nativeIP->dup();

                    ev << " Swap label(" << oldLabel << ") by label(" << newLabel << ")\n";

                    ev << " Sending the packet to inteface "<< outgoingInterface.c_str() <<"\n";
                    send(dupIPDatagram, "toL2", outgoingPort);
                    delete nativeIP;
                }
                else    //Message is out of the tunnel
                {
                    ev << " Swap label(" << oldLabel << ") by label(" << newLabel << ")\n";
                    ev << " Sending the packet to inteface "<< outgoingInterface.c_str() <<"\n";
                    send(mplsPacket, "toL2", outgoingPort);
                }
            }
            else //Some sort of error here
            {
                error("LIB table inconsistance");
            }
        }
        else //Decapsulate the message and pass up to L3 since this is LDP packet
        {
            IPDatagram *ipdatagram = check_and_cast<IPDatagram *>(mplsPacket->decapsulate());
            IPDatagram *dupPack = (IPDatagram *) ipdatagram->dup();
            send(dupPack, "toL3", gateIndex);
            delete msg;
        }
    }
    else if (isIR) //Data from outside for IR host
    {
        // FIXME something seems wrong. If above, in the "if (mplsPacket !=NULL)"
        // branch we processed all MPLS packets, then here in this "else" brach
        // it makes no sense to refer to mplsPacket any more!!!
        if (ipdata->hasPar("trans"))
        {
           send(ipdata, "toL3", gateIndex);
        }
        else
        {
            //Incoming interface
            InterfaceEntry* ientry= rt->getInterfaceByIndex(gateIndex);
            string senderInterface =string(ientry->name.c_str());
            ev << " Message from outside to Ingress node" << "\n";
            if (mplsPacket!=NULL)
            {
                ipdata = check_and_cast<IPDatagram*>(mplsPacket->decapsulate());
            }
            //Add color for this fec path
            //std::list<fec_color_mapping>::iterator iterF;
            //fec_color_mapping iter;
            bool makeRequest = false;
            int fecID = classifyPacket(ipdata, classifierType);
            //int myColor =0;
            if (fecID ==-1)
            {
                makeRequest = true;
                fecID =  classifyPacket(ipdata, classifierType);
                ev << "My LSP id mapping is " << fecID << "\n";
                //fec_color_mapping *colorMap = new fec_color_mapping;
                //colorMap->fecId = fecID;
                //colorMap->color = pathColor.size();
                //pathColor.push_back(*colorMap);
            }
            /*
            else
            {
                std::list<fec_color_mapping>::iterator iterF;

                fec_color_mapping iter;

                for (iterF = pathColor.begin(); iterF!= pathColor.end(); iterF++)
                {
                    iter = (fec_color_mapping)*iterF;
                    if (iter.fecId == fecID)
                    myColor = iter.color;
                    break;
                }
            }
            */
            ev << "Message(src, dest, fec)=(" << ipdata->srcAddress() << "," <<
              ipdata->destAddress() << "," << fecID << ")\n";

            int label = lt->requestLabelforFec(fecID);
            ev << " Label found for this message is label(" << label << ")\n";

            if (label!=-2) //New Label found
            {
                //Construct a new MPLS packet

                MPLSPacket* newPacket=NULL;

                if (mplsPacket == NULL)
                {
                    newPacket =new MPLSPacket();
                    newPacket->encapsulate(ipdata);
                }
                else
                    newPacket = mplsPacket;

                //consistent in packet color
                if (fecID < MAX_LSP_NO)
                    newPacket->setKind(fecID);
                else
                    newPacket->setKind(2*MAX_LSP_NO-fecID);

                newPacket->pushLabel(label);

                //Find outgoing interface

                //string outgoingInterface= lt->requestOutgoingInterface(senderInterface, label);
                string outgoingInterface = lt->requestOutgoingInterface(fecID);

                int outgoingPort =rt->findInterfaceByName(outgoingInterface.c_str());

                //Send out the packet
                ev << " Ingress Node push new label and sending this packet\n";
                send(newPacket, "toL2", outgoingPort);
                //delete msg;
            }
            else //Need to make ldp query
            {
                if (ipdata!=NULL)
                {
                    ipdata->addPar("gateIndex")=gateIndex;
                    ipdataQueue.add(ipdata);
                }
                else if (mplsPacket!=NULL)
                {
                    mplsPacket->addPar("gateIndex")=gateIndex;
                    ipdataQueue.add(mplsPacket);
                }

                //Whether I made requests for this FEC
                if (!makeRequest)
                    return; //Do nothing since I have made a previous request pending

                //signal the LDP by sending some messages
                cMessage *signalMessage =new cMessage("toSignallingModule");
                //signalMessage->setKind(SIGNAL_KIND);
                signalMessage->addPar("FEC") = fecID;
                signalMessage->addPar("dest_addr")=IPAddress(ipdata->destAddress()).getInt();
                signalMessage->addPar("src_addr") = IPAddress(ipdata->srcAddress()).getInt();
                signalMessage->addPar("gateIndex") = gateIndex;

                if (isSignallingReady)
                {
                    //Send to MPLSSwitch
                    sendDirect(signalMessage, 0.0, ldpMod, "from_mpls_switch");
                }
                else //Pending
                {
                    ldpQueue.add(signalMessage);
                }
            }//End query making

        }//End data is not transparency
    }
    else
    {
        error("Unknown message received");
    }
}



int MPLSModule::classifyPacket(IPDatagram* ipdata, int type)
{
    int src = IPAddress(ipdata->destAddress()).getInt();
    int dest = IPAddress(ipdata->srcAddress()).getInt();

    std::vector<fec>::iterator iterF;
    fec iter;
    int fec_id =0;

    for (iterF = fecList.begin(); iterF!=fecList.end();iterF++)
    {
        iter = (fec)*iterF;

        // FEC determined by Destination only
        if ((type == DEST_CLASSIFIER) && (dest == iter.dest))
            return iter.fecId;

        // FEC determined by Destination and Source
        if ((type == DEST_SOURCE_CLASSIFIER) && (dest == iter.dest) && (src == iter.src))
            return iter.fecId;

        if (fec_id <= iter.fecId)
            fec_id = iter.fecId +1;
    }

    fec* newEle = new fec;
    newEle->dest = dest;
    newEle->src =src;
    newEle->fecId = fec_id;
    fecList.push_back(*newEle);
    return -1;
}









