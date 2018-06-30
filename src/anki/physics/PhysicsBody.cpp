// Copyright (C) 2009-2018, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <anki/physics/PhysicsBody.h>
#include <anki/physics/PhysicsWorld.h>
#include <anki/physics/PhysicsCollisionShape.h>

namespace anki
{

class PhysicsBody::MotionState : public btMotionState
{
public:
	PhysicsBody* m_body = nullptr;

	void getWorldTransform(btTransform& worldTrans) const override
	{
		worldTrans = toBt(m_body->m_trf);
	}

	void setWorldTransform(const btTransform& worldTrans) override
	{
		m_body->m_trf = toAnki(worldTrans);
	}
};

PhysicsBody::PhysicsBody(PhysicsWorld* world, const PhysicsBodyInitInfo& init)
	: PhysicsFilteredObject(CLASS_TYPE, world)
{
	const Bool dynamic = init.m_mass > 0.0f;
	m_shape = init.m_shape;

	// Create motion state
	m_motionState = getAllocator().newInstance<MotionState>();
	m_motionState->m_body = this;

	// Compute inertia
	btCollisionShape* shape = m_shape->getBtShape(dynamic);
	btVector3 localInertia(0, 0, 0);
	if(dynamic)
	{
		shape->calculateLocalInertia(init.m_mass, localInertia);
	}

	// Create body
	btRigidBody::btRigidBodyConstructionInfo cInfo(init.m_mass, m_motionState, shape, localInertia);
	cInfo.m_friction = init.m_friction;
	m_body = getAllocator().newInstance<btRigidBody>(cInfo);

	// User pointer
	m_body->setUserPointer(static_cast<PhysicsObject*>(this));

	// Other
	setMaterialGroup(PhysicsMaterialBit::DYNAMIC_GEOMETRY | PhysicsMaterialBit::STATIC_GEOMETRY);
	setMaterialMask(PhysicsMaterialBit::ALL);

	// Add to world
	auto lock = getWorld().lockBtWorld();
	getWorld().getBtWorld()->addRigidBody(m_body);
}

PhysicsBody::~PhysicsBody()
{
	if(m_body)
	{
		auto lock = getWorld().lockBtWorld();
		getWorld().getBtWorld()->removeRigidBody(m_body);
	}

	getAllocator().deleteInstance(m_body);
	getAllocator().deleteInstance(m_motionState);
}

} // end namespace anki
